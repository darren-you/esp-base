// SPDX-License-Identifier: Apache-2.0
// 应用槽、部署记录和下载任务。
//
// 应用槽不在分区表里：启动时把 Flash 末尾空闲的 64 KiB 登记成两个 32 KiB 的数据分区，现有设备不用重刷
// 分区表。应用包原样放在槽的开头；每次读槽都重新核对包头、清单、签名和摘要，槽里的内容不可信时就当它是空的。
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_wasm_internal.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define SLOT_SUBTYPE 0x40 /* 自定义的数据分区子类型 */
#define HEAD_BYTES (WASM_PACKAGE_HEADER_BYTES + WASM_PACKAGE_MANIFEST_MAX)
#define CHUNK_BYTES 1024
#define FETCH_STACK_BYTES 8192 /* TLS 握手和验签都在这个任务的栈上 */
#define CONNECT_TIMEOUT_MS 20000
#define RECORD_NAMESPACE "app"
#define RECORD_KEY "deploy"
#if CONFIG_ESP_BASE_TEST_WASM_DEPLOY
#define STAGED_PREFIX "https://staged.test/"
#define STAGED_OFFSET 0x200000u /* 测试固件：待部署的包预先放在 ota_1 里，每个占一个槽的大小 */
#endif

static const char *TAG = "base_wasm";
static const esp_partition_t *s_slots[WASM_SLOTS];
/* 下载任务用的一份期望应用：下载期间才占内存（600 多字节），不常驻。 */
static struct fetch_job {
    wasm_desired_t desired;
    uint8_t slot;
} *s_job;
static wasm_fetch_result_t s_result;
static atomic_bool s_fetching, s_cancel;

bool wasm_store_init(void)
{
    static const char *const labels[WASM_SLOTS] = {"app_a", "app_b"};
    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK || flash_bytes < WASM_SLOTS * WASM_SLOT_BYTES) return false;
    for (unsigned index = 0; index < WASM_SLOTS; ++index) {
        if (s_slots[index]) continue;
        const uint32_t offset = flash_bytes - (WASM_SLOTS - index) * WASM_SLOT_BYTES;
        const esp_err_t result = esp_partition_register_external(NULL, offset, WASM_SLOT_BYTES, labels[index],
                                                                 ESP_PARTITION_TYPE_DATA, SLOT_SUBTYPE, &s_slots[index]);
        if (result != ESP_OK) {
            /* 分区表占用了 Flash 末尾：这个固件上没有地方放应用。 */
            ESP_LOGE(TAG, "ESP_BASE_APP_NO_SLOTS %s at 0x%x: %s", labels[index], (unsigned)offset, esp_err_to_name(result));
            return false;
        }
    }
    return true;
}

/* 读回分区 [0, length) 计算 SHA-256：校验的是 Flash 里实际的字节。 */
static bool partition_sha256(const esp_partition_t *partition, uint32_t length, char out[WASM_SHA256_HEX_BYTES])
{
    uint8_t *chunk = malloc(CHUNK_BYTES);
    uint8_t digest[32];
    size_t digest_length = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool ok = chunk && psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t done = 0; ok && done < length;) {
        const uint32_t bytes = length - done < CHUNK_BYTES ? length - done : CHUNK_BYTES;
        ok = esp_partition_read(partition, done, chunk, bytes) == ESP_OK && psa_hash_update(&hash, chunk, bytes) == PSA_SUCCESS;
        done += bytes;
    }
    ok = ok && psa_hash_finish(&hash, digest, sizeof digest, &digest_length) == PSA_SUCCESS && digest_length == 32;
    if (!ok) psa_hash_abort(&hash);
    free(chunk);
    for (int index = 0; ok && index < 32; ++index) snprintf(out + index * 2, 3, "%02x", digest[index]);
    return ok;
}

bool wasm_store_inspect(uint8_t index, wasm_slot_t *slot, wasm_package_t *package, const char **error)
{
    memset(slot, 0, sizeof *slot);
    memset(package, 0, sizeof *package);
    const esp_partition_t *partition = index < WASM_SLOTS ? s_slots[index] : NULL;
    uint8_t *head = malloc(HEAD_BYTES);
    const char *reason = "storage";
    if (partition && head && esp_partition_read(partition, 0, head, HEAD_BYTES) == ESP_OK) {
        reason = wasm_package_parse(head, HEAD_BYTES, partition->size, package);
        /* 与固件 OTA 相同的验签：信任根是当前运行固件签名块里的公钥。 */
        if (!reason && esp_secure_boot_verify_signature(partition->address, package->signed_bytes) != ESP_OK)
            reason = "signature_invalid";
        if (!reason) reason = wasm_package_accept(package, CONFIG_IDF_TARGET, WASM_MEMORY_MAX_BYTES);
        if (!reason && !partition_sha256(partition, package->total_bytes, slot->sha256)) reason = "storage";
    }
    free(head);
    if (reason) {
        memset(slot, 0, sizeof *slot);
        memset(package, 0, sizeof *package);
        if (error) *error = reason;
        return false;
    }
    slot->valid = true;
    slot->abi = package->abi;
    snprintf(slot->name, sizeof slot->name, "%s", package->name);
    snprintf(slot->version, sizeof slot->version, "%s", package->version);
    return true;
}

bool wasm_store_read(uint8_t index, uint32_t offset, uint8_t *out, uint32_t bytes)
{
    return index < WASM_SLOTS && s_slots[index] && esp_partition_read(s_slots[index], offset, out, bytes) == ESP_OK;
}

bool wasm_store_load_record(wasm_deploy_record_t *out)
{
    nvs_handle_t handle;
    size_t size = sizeof *out;
    if (nvs_open(RECORD_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    const bool ok = nvs_get_blob(handle, RECORD_KEY, out, &size) == ESP_OK && size == sizeof *out;
    nvs_close(handle);
    return ok;
}

void wasm_store_save_record(const wasm_deploy_record_t *record)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(RECORD_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, RECORD_KEY, record, sizeof *record);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) ESP_LOGE(TAG, "deploy record not saved: %s", esp_err_to_name(result));
}

/* ---------- 下载任务 ---------- */

const wasm_fetch_result_t *wasm_fetch_result(void)
{
    return &s_result;
}

void wasm_fetch_cancel(void)
{
    atomic_store(&s_cancel, true);
}

static void finish(wasm_worker_result_t result, const char *error)
{
    s_result.result = result;
    snprintf(s_result.error, sizeof s_result.error, "%s", error ? error : "");
    free(s_job);
    s_job = NULL;
    atomic_store(&s_fetching, false);
    base_wasm_post(&(base_wasm_event_t){.kind = EV_FETCH_DONE});
}

/* 把包写进槽。返回 NULL 表示写完了 size 字节；否则是原因，*result 是该重试还是拒绝。 */
static const char *download(const esp_partition_t *partition, uint8_t *chunk, wasm_worker_result_t *result)
{
    static char rejected[WASM_ERROR_BYTES];
    const uint32_t size = s_job->desired.size;
    uint32_t written = 0;
    *result = WASM_WORKER_RETRY;
#if CONFIG_ESP_BASE_TEST_WASM_DEPLOY
    if (!strncmp(s_job->desired.url, STAGED_PREFIX, sizeof STAGED_PREFIX - 1)) {
        const uint32_t source = STAGED_OFFSET + (uint32_t)atoi(s_job->desired.url + sizeof STAGED_PREFIX - 1) * WASM_SLOT_BYTES;
        for (; written < size; written += CHUNK_BYTES) {
            const uint32_t bytes = size - written < CHUNK_BYTES ? size - written : CHUNK_BYTES;
            if (esp_flash_read(NULL, chunk, source + written, bytes) != ESP_OK ||
                esp_partition_write(partition, written, chunk, bytes) != ESP_OK)
                return "storage";
        }
        return NULL;
    }
#endif
    const esp_http_client_config_t http = {
        .url = s_job->desired.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .buffer_size = CHUNK_BYTES,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    const char *error = NULL;
    if (!client || esp_http_client_open(client, 0) != ESP_OK) {
        error = "connect";
    } else {
        const int64_t length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status >= 400 && status < 500 && status != 408 && status != 429) {
            /* 403、404 等客户端错误不可恢复；其余按网络错误重试。 */
            snprintf(rejected, sizeof rejected, "http_%d", status);
            *result = WASM_WORKER_REJECT;
            error = rejected;
        } else if (status != 200) {
            error = "http";
        } else if (length > 0 && length != (int64_t)size) {
            *result = WASM_WORKER_REJECT;
            error = "size_mismatch";
        }
        while (!error && written < size) {
            if (atomic_load(&s_cancel)) {
                *result = WASM_WORKER_CANCELED;
                error = "canceled";
                break;
            }
            const uint32_t room = size - written < CHUNK_BYTES ? size - written : CHUNK_BYTES;
            const int bytes = esp_http_client_read(client, (char *)chunk, (int)room);
            if (bytes <= 0) error = "transport"; /* 连接断开、读超时，或者对端给的比说好的少 */
            else if (esp_partition_write(partition, written, chunk, (size_t)bytes) != ESP_OK) error = "storage";
            else written += (uint32_t)bytes;
        }
        /* 对端给的比说好的多：不是我们要的那个文件。 */
        if (!error && esp_http_client_read(client, (char *)chunk, 1) > 0) {
            *result = WASM_WORKER_REJECT;
            error = "size_mismatch";
        }
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return error;
}

static void fetch_task(void *argument)
{
    (void)argument;
    const esp_partition_t *partition = s_slots[s_job->slot];
    wasm_worker_result_t result = WASM_WORKER_RETRY;
    uint8_t *chunk = malloc(CHUNK_BYTES);
    const char *error = !chunk || esp_partition_erase_range(partition, 0, partition->size) != ESP_OK ? "storage" : NULL;
    if (!error) error = download(partition, chunk, &result);
    free(chunk);
    if (error) {
        finish(result, result == WASM_WORKER_CANCELED ? NULL : error);
        return;
    }
    base_wasm_post(&(base_wasm_event_t){.kind = EV_FETCH_VERIFYING});
    /* 先核对摘要（Flash 里实际的字节），再核对包头、清单、签名和这个固件能否运行。 */
    char actual[WASM_SHA256_HEX_BYTES] = "";
    if (!partition_sha256(partition, s_job->desired.size, actual)) {
        finish(WASM_WORKER_RETRY, "storage");
    } else if (strcmp(actual, s_job->desired.sha256) != 0) {
        ESP_LOGE(TAG, "app package sha256 mismatch: expected %s, flash has %s", s_job->desired.sha256, actual);
        finish(WASM_WORKER_REJECT, "sha256_mismatch");
    } else if (!wasm_store_inspect(s_job->slot, &s_result.installed, &s_result.package, &error)) {
        finish(WASM_WORKER_REJECT, error);
    } else if (s_result.package.total_bytes != s_job->desired.size) {
        finish(WASM_WORKER_REJECT, "size_mismatch");
    } else {
        finish(WASM_WORKER_INSTALLED, NULL);
    }
}

bool wasm_fetch_start(const wasm_desired_t *desired, uint8_t slot)
{
    if (slot >= WASM_SLOTS || !s_slots[slot] || desired->size > WASM_SLOT_BYTES) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_fetching, &expected, true)) return false;
    atomic_store(&s_cancel, false);
    memset(&s_result, 0, sizeof s_result);
    if ((s_job = malloc(sizeof *s_job))) {
        s_job->desired = *desired;
        s_job->slot = slot;
        if (base_wasm_task("app_fetch", fetch_task, FETCH_STACK_BYTES, 0)) return true; /* 下载自己有超时，不设看门狗 */
        free(s_job);
        s_job = NULL;
    }
    atomic_store(&s_fetching, false);
    return false;
}
