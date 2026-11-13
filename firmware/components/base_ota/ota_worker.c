// SPDX-License-Identifier: Apache-2.0
// 下载任务：官方 esp_https_ota 按 Range 续传并顺序写备用槽；整镜像 SHA-256 与签名都通过后才切换启动槽。
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "base_ota.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define WORKER_STACK_BYTES 8192
#define HASH_CHUNK_BYTES 4096
#define SECTOR_BYTES 4096u

static const char *TAG = "base_ota";

static struct {
    char url[BASE_URL_BYTES];
    char sha256[BASE_SHA256_HEX_BYTES];
    uint32_t size;
    uint32_t offset;
    base_ota_worker_cb_t callback;
    int http_status;
} s_job;
static atomic_bool s_running, s_cancel;

static void to_hex(const uint8_t digest[32], char out[BASE_SHA256_HEX_BYTES])
{
    for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

/* 读回分区 [0, length) 计算 SHA-256：校验的是 Flash 里实际的字节，断电续传后同样成立。 */
static bool partition_sha256(const esp_partition_t *partition, uint32_t length, char out[BASE_SHA256_HEX_BYTES])
{
    uint8_t *chunk = malloc(HASH_CHUNK_BYTES);
    uint8_t digest[32];
    size_t digest_len = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool ok = chunk && psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t done = 0; ok && done < length;) {
        const uint32_t n = length - done < HASH_CHUNK_BYTES ? length - done : HASH_CHUNK_BYTES;
        ok = esp_partition_read(partition, done, chunk, n) == ESP_OK &&
             psa_hash_update(&hash, chunk, n) == PSA_SUCCESS;
        done += n;
    }
    ok = ok && psa_hash_finish(&hash, digest, sizeof digest, &digest_len) == PSA_SUCCESS && digest_len == 32;
    if (!ok) psa_hash_abort(&hash);
    free(chunk);
    if (ok) to_hex(digest, out);
    return ok;
}

bool base_ota_running_firmware(base_fw_t *out, uint32_t *slot_bytes, bool *pending_verify)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!running || !next || !out || !slot_bytes || !pending_verify) return false;
    memset(out, 0, sizeof *out);
    const esp_partition_pos_t position = {.offset = running->address, .size = running->size};
    esp_image_metadata_t metadata = {0};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK) return false;
    /* signed bin = 镜像补齐到 4 KiB，再加一个扇区的签名块；image_len 已含签名块。 */
    const uint32_t signed_len = (metadata.image_len + SECTOR_BYTES - 1) & ~(SECTOR_BYTES - 1);
    if (signed_len > running->size || !partition_sha256(running, signed_len, out->sha256)) return false;
    snprintf(out->version, sizeof out->version, "%s", esp_app_get_description()->version);
    snprintf(out->target, sizeof out->target, "%s/%s", CONFIG_IDF_TARGET, esp_app_get_description()->project_name);
    snprintf(out->slot, sizeof out->slot, "%s", running->label);
    *slot_bytes = next->size;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    *pending_verify = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE)
        s_job.http_status = esp_http_client_get_status_code(event->client);
    return ESP_OK;
}

static void done(base_ota_result_t result, const char *error) __attribute__((noreturn));
static void done(base_ota_result_t result, const char *error)
{
    const base_ota_worker_cb_t callback = s_job.callback;
    atomic_store(&s_running, false);
    callback(BASE_OTA_WORKER_DONE, (uint32_t)result, error);
    vTaskDelete(NULL);
    for (;;) {}
}

static void worker(void *argument)
{
    (void)argument;
    static char error[BASE_ERROR_BYTES];
    const esp_http_client_config_t http = {
        .url = s_job.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .event_handler = http_event,
    };
    const esp_https_ota_config_t config = {
        .http_config = &http,
        .ota_resumption = true,
        .ota_image_bytes_written = s_job.offset,
    };
    esp_https_ota_handle_t handle = NULL;
    s_job.http_status = 0;
    esp_err_t result = esp_https_ota_begin(&config, &handle);
    if (result != ESP_OK) {
        /* 403／404 等客户端错误不可恢复；408、429 和其余情况按网络错误重试。 */
        const int status = s_job.http_status;
        if (status >= 400 && status < 500 && status != 408 && status != 429) {
            snprintf(error, sizeof error, "http_%d", status);
            done(BASE_OTA_RESULT_REJECT, error);
        }
        done(BASE_OTA_RESULT_RETRY, result == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? "not_confirmed" : "connect");
    }
    uint32_t reported = s_job.offset;
    for (;;) {
        if (atomic_load(&s_cancel)) {
            esp_https_ota_abort(handle);
            done(BASE_OTA_RESULT_CANCELED, NULL);
        }
        result = esp_https_ota_perform(handle);
        if (result != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        const uint32_t written = (uint32_t)esp_https_ota_get_image_len_read(handle);
        if (written > s_job.size) {
            esp_https_ota_abort(handle);
            done(BASE_OTA_RESULT_REJECT, "size_mismatch");
        }
        if (written / BASE_OTA_PERSIST_STEP_BYTES != reported / BASE_OTA_PERSIST_STEP_BYTES) {
            reported = written;
            s_job.callback(BASE_OTA_WORKER_PROGRESS, written, NULL);
        }
    }
    if (result != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        const bool invalid = result == ESP_ERR_OTA_VALIDATE_FAILED;
        esp_https_ota_abort(handle);
        done(invalid ? BASE_OTA_RESULT_REJECT : BASE_OTA_RESULT_RETRY, invalid ? "image_invalid" : "transport");
    }
    if ((uint32_t)esp_https_ota_get_image_len_read(handle) != s_job.size) {
        esp_https_ota_abort(handle);
        done(BASE_OTA_RESULT_REJECT, "size_mismatch");
    }
    s_job.callback(BASE_OTA_WORKER_VERIFYING, s_job.size, NULL);
    char actual[BASE_SHA256_HEX_BYTES] = {0};
    const esp_partition_t *staging = esp_ota_get_next_update_partition(NULL);
    if (!staging || !partition_sha256(staging, s_job.size, actual)) {
        esp_https_ota_abort(handle);
        done(BASE_OTA_RESULT_RETRY, "flash_read");
    }
    if (strcmp(actual, s_job.sha256) != 0) {
        ESP_LOGE(TAG, "sha256 mismatch: expected %s, flash has %s", s_job.sha256, actual);
        esp_https_ota_abort(handle);
        done(BASE_OTA_RESULT_REJECT, "sha256_mismatch");
    }
    /* 官方校验：镜像结构、芯片型号、内嵌摘要和 RSA 签名；全部通过才切换启动槽。 */
    result = esp_https_ota_finish(handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(result));
        done(BASE_OTA_RESULT_REJECT, "image_invalid");
    }
    done(BASE_OTA_RESULT_SWITCHED, NULL);
}

bool base_ota_worker_start(const char *url, const char *sha256, uint32_t size, uint32_t offset,
                           base_ota_worker_cb_t callback)
{
    if (!url || !sha256 || !callback || strlen(url) >= sizeof s_job.url ||
        !base_is_sha256_hex(sha256) || offset > size) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_running, &expected, true)) return false;
    atomic_store(&s_cancel, false);
    strcpy(s_job.url, url);
    strcpy(s_job.sha256, sha256);
    s_job.size = size;
    s_job.offset = offset;
    s_job.callback = callback;
    if (xTaskCreate(worker, "ota", WORKER_STACK_BYTES, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        atomic_store(&s_running, false);
        return false;
    }
    return true;
}

void base_ota_worker_cancel(void)
{
    atomic_store(&s_cancel, true);
}
