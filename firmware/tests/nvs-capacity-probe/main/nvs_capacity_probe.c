// SPDX-License-Identifier: Apache-2.0
// Synthetic QEMU-only capacity probe. No device backup or credential is used.
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_base_remote_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#define PARTITION "base_store"
#define OTA_NAMESPACE "base_ota"
#define OTA_KEY "operation"
#define CONTAINER_NAMESPACE "base_container"
#define CONTAINER_KEY "state"
#define OTA_BYTES 118U
#define CONTAINER_BYTES 288U
#define FINAL_REVISION 100U

static esp_base_remote_config_t current_config;
static esp_base_remote_config_t candidate_config;
static esp_base_remote_config_t committed_config;
static esp_base_remote_config_t work_config;
static uint8_t canonical_bytes[EBASE_CONFIG_MAX_BYTES];
static uint8_t ota_bytes[OTA_BYTES];
static uint8_t container_bytes[CONTAINER_BYTES];
static uint8_t side_readback[CONTAINER_BYTES];
static uint8_t previous_digest[32];

static void put_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t get_u32(const uint8_t *bytes)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)bytes[i] << (i * 8);
    return value;
}

static void fill_host(char host[254])
{
    for (unsigned i = 0; i < 253; ++i)
        host[i] = (i == 63 || i == 127 || i == 191) ? '.' : 'a';
    host[253] = 0;
}

static bool make_max_config(esp_base_remote_config_t *config)
{
    memset(config, 0, sizeof *config);
    config->wifi.configured = true;
    memset(config->wifi.ssid, 's', 32);
    memset(config->wifi.password, 'a', 64);
    config->mqtt.configured = true;
    fill_host(config->mqtt.hostname);
    config->mqtt.port = 8883;
    memset(config->mqtt.username, 'u', 128);
    memset(config->mqtt.password, 'p', 256);
    memset(config->mqtt.ca_pem, 'A', 4096);
    memcpy(config->mqtt.ca_pem, "-----BEGIN CERTIFICATE-----", 27);
    memcpy(config->mqtt.ca_pem + 4096 - 25, "-----END CERTIFICATE-----", 25);
    config->mqtt.management_key[0] = 1;
    config->frp.configured = true;
    fill_host(config->frp.server_hostname);
    config->frp.server_port = 7000;
    memset(config->frp.token, 't', EBASE_FRP_TOKEN_MAX_BYTES);
    memset(config->frp.ca_pem, 'A', EBASE_FRP_CA_MAX_BYTES);
    memcpy(config->frp.ca_pem, "-----BEGIN CERTIFICATE-----", 27);
    memcpy(config->frp.ca_pem + EBASE_FRP_CA_MAX_BYTES - 25,
           "-----END CERTIFICATE-----", 25);
    memset(config->frp.proxy_name, 'p', 128);
    config->frp.remote_port = 443;
    config->frp.local_port = 8080;
    config->frp.management_key[0] = 1;
    size_t size = 0;
    return ebase_config_valid(config) &&
           ebase_config_encode(config, canonical_bytes, &size) &&
           size == EBASE_CONFIG_MAX_BYTES;
}

static void make_ota(uint32_t revision)
{
    memset(ota_bytes, 0, sizeof ota_bytes);
    memcpy(ota_bytes, "EOTA", 4);
    ota_bytes[4] = 1;       // v1
    ota_bytes[5] = 1;       // PREPARED
    ota_bytes[6] = 0x10;    // OTA_0
    ota_bytes[7] = 0x11;    // OTA_1
    put_u32(ota_bytes + 10, 0x100000);
    static const char synthetic_id[] = "00000000-0000-4000-8000-000000000000";
    memcpy(ota_bytes + 14, synthetic_id, 36);
    memcpy(ota_bytes + 50, synthetic_id, 36);
    for (unsigned i = 86; i < OTA_BYTES; ++i) ota_bytes[i] = (uint8_t)(i * 7U);
    put_u32(ota_bytes + 86, revision);
}

static void make_container(uint32_t revision)
{
    // Container's future product namespace/key is not frozen. Only exact NVS
    // blob length and write/readback pressure are tested here.
    for (unsigned i = 0; i < CONTAINER_BYTES; ++i)
        container_bytes[i] = (uint8_t)(i * 11U + 3U);
    memcpy(container_bytes, "ECAP", 4);
    put_u32(container_bytes + 4, revision);
}

static esp_err_t write_and_check(const char *name, const char *key,
                                  const uint8_t *bytes, size_t size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition(PARTITION, name, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, key, bytes, size);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    error = nvs_open_from_partition(PARTITION, name, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t received = sizeof side_readback;
    error = nvs_get_blob(handle, key, side_readback, &received);
    nvs_close(handle);
    return error == ESP_OK && received == size && memcmp(bytes, side_readback, size) == 0 ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t read_side(const char *name, const char *key, uint8_t *bytes,
                           size_t size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition(PARTITION, name, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t received = size;
    error = nvs_get_blob(handle, key, bytes, &received);
    nvs_close(handle);
    return error == ESP_OK && received == size ? ESP_OK :
           error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
}

static bool digest_consumer(const uint8_t *bytes, size_t size, void *context)
{
    size_t written = 0;
    return size == EBASE_CONFIG_MAX_BYTES &&
           psa_hash_compute(PSA_ALG_SHA_256, bytes, size, context, 32,
                            &written) == PSA_SUCCESS && written == 32;
}

static void print_digest(const uint8_t digest[32])
{
    for (unsigned i = 0; i < 32; ++i) printf("%02x", digest[i]);
}

static void print_stats(uint32_t revision)
{
    nvs_stats_t stats;
    const esp_err_t error = nvs_get_stats(PARTITION, &stats);
    if (error == ESP_OK)
        printf("PROBE_STATS=%" PRIu32 " used=%u free=%u available=%u total=%u namespaces=%u\n",
               revision, (unsigned)stats.used_entries, (unsigned)stats.free_entries,
               (unsigned)stats.available_entries, (unsigned)stats.total_entries,
               (unsigned)stats.namespace_count);
    else
        printf("PROBE_STATS_ERROR=%" PRIu32 " code=%d\n", revision, (int)error);
}

static bool run_steps(uint32_t first, uint32_t last)
{
    bool have_previous = current_config.revision != 0;
    if (have_previous &&
        !esp_base_remote_config_with_canonical_bytes(&current_config,
                                                      digest_consumer, previous_digest)) {
        printf("PROBE_FAIL=prior_digest\n");
        return false;
    }
    for (uint32_t revision = first; revision <= last; ++revision) {
        candidate_config = current_config;
        candidate_config.mqtt.management_key[1] = (uint8_t)revision;
        candidate_config.frp.management_key[1] = (uint8_t)(revision ^ 0x5aU);
        const esp_err_t error = esp_base_remote_config_commit_verified(
            &candidate_config, current_config.revision, &committed_config, &work_config);
        if (error != ESP_OK) {
            printf("PROBE_FAIL=config desired=%" PRIu32 " code=%d\n", revision, (int)error);
            return false;
        }
        current_config = committed_config;
        uint8_t digest[32];
        if (current_config.revision != revision ||
            !esp_base_remote_config_with_canonical_bytes(&current_config,
                                                          digest_consumer, digest) ||
            (have_previous && memcmp(previous_digest, digest, sizeof digest) == 0)) {
            printf("PROBE_FAIL=config_readback desired=%" PRIu32 "\n", revision);
            return false;
        }
        make_ota(revision);
        esp_err_t side_error = write_and_check(OTA_NAMESPACE, OTA_KEY,
                                               ota_bytes, sizeof ota_bytes);
        if (side_error != ESP_OK) {
            printf("PROBE_FAIL=ota desired=%" PRIu32 " code=%d\n",
                   revision, (int)side_error);
            return false;
        }
        make_container(revision);
        side_error = write_and_check(CONTAINER_NAMESPACE, CONTAINER_KEY,
                                     container_bytes, sizeof container_bytes);
        if (side_error != ESP_OK) {
            printf("PROBE_FAIL=container desired=%" PRIu32 " code=%d\n",
                   revision, (int)side_error);
            return false;
        }
        printf("PROBE_STEP=%" PRIu32 " config_sha256=", revision);
        print_digest(digest);
        printf(" ota=ok container=ok\n");
        memcpy(previous_digest, digest, sizeof digest);
        have_previous = true;
        print_stats(revision);
    }
    return true;
}

static bool verify_reboot(void)
{
    uint8_t digest[32], observed_ota[OTA_BYTES], observed_container[CONTAINER_BYTES];
    const bool config_valid =
        ebase_config_valid(&current_config) &&
        esp_base_remote_config_with_canonical_bytes(&current_config,
                                                     digest_consumer, digest);
    const esp_err_t ota_error = read_side(OTA_NAMESPACE, OTA_KEY, observed_ota,
                                          sizeof observed_ota);
    const esp_err_t container_error = read_side(CONTAINER_NAMESPACE, CONTAINER_KEY,
                                                observed_container,
                                                sizeof observed_container);
    const uint32_t ota_revision = ota_error == ESP_OK ? get_u32(observed_ota + 86) : 0;
    const uint32_t container_revision =
        container_error == ESP_OK ? get_u32(observed_container + 4) : 0;
    printf("PROBE_RESTART_CONFIG_REV=%" PRIu32 " valid=%d sha256=",
           current_config.revision, config_valid);
    if (config_valid) print_digest(digest);
    else printf("unavailable");
    printf("\nPROBE_RESTART_OTA_REV=%" PRIu32 " code=%d\n",
           ota_revision, (int)ota_error);
    printf("PROBE_RESTART_CONTAINER_REV=%" PRIu32 " code=%d\n",
           container_revision, (int)container_error);
    if (ota_error == ESP_OK) make_ota(ota_revision);
    if (container_error == ESP_OK) make_container(container_revision);
    const bool intact = config_valid && ota_error == ESP_OK &&
        container_error == ESP_OK &&
        memcmp(observed_ota, ota_bytes, OTA_BYTES) == 0 &&
        memcmp(observed_container, container_bytes, CONTAINER_BYTES) == 0 &&
        ota_revision == current_config.revision &&
        container_revision == current_config.revision;
    printf("PROBE_RESTART_MATCH=%d\n", intact);
    print_stats(current_config.revision);
    return intact;
}

void app_main(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("PROBE_FAIL=crypto_init\n");
        goto done;
    }
    const esp_err_t init_error = nvs_flash_init_partition(PARTITION);
    printf("PROBE_INIT=%d stage=%d\n", (int)init_error, PROBE_STAGE);
    if (init_error != ESP_OK) goto done;
    const esp_err_t load_error = esp_base_remote_config_load(&current_config);
    printf("PROBE_LOAD=%d revision=%" PRIu32 "\n",
           (int)load_error, current_config.revision);
    if (load_error != ESP_OK) goto done;
#if PROBE_STAGE == 1
    if (current_config.revision != 0 || !make_max_config(&candidate_config)) {
        printf("PROBE_FAIL=initial_state\n");
        goto done;
    }
    current_config = candidate_config;
    if (!run_steps(1, 3)) goto done;
    candidate_config = current_config;
    candidate_config.revision = 2;
    const esp_err_t conflict = esp_base_remote_config_commit_verified(
        &candidate_config, 2, &committed_config, &work_config);
    printf("PROBE_STALE_CAS=%d expected=%d\n", (int)conflict,
           ESP_BASE_CONFIG_CONFLICT);
    if (conflict != ESP_BASE_CONFIG_CONFLICT) {
        printf("PROBE_FAIL=stale_cas code=%d\n", (int)conflict);
        goto done;
    }
    printf("PROBE_DONE=stage1\n");
#elif PROBE_STAGE == 2
    if (current_config.revision != 3) {
        printf("PROBE_FAIL=stage2_prior_revision\n");
        goto done;
    }
    if (!verify_reboot()) goto done;
    if (!run_steps(4, FINAL_REVISION)) goto done;
    printf("PROBE_DONE=stage2\n");
#elif PROBE_STAGE == 3
    if (!verify_reboot()) goto done;
    printf("PROBE_DONE=stage3\n");
#endif
done:
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
