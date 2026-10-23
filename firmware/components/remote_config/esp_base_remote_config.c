// SPDX-License-Identifier: Apache-2.0
#include "esp_base_remote_config.h"
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#define CONFIG_PARTITION "base_store"
#define CONFIG_NAMESPACE "base_config"
#define CONFIG_KEY "committed"

/* Startup and the single control task own the codec workspace. On C3 the
 * live config occupies RTC; the codec owns one bounded ordinary-RAM buffer.
 * ESP32 keeps its existing RTC workspace and byte-accessible IRAM config. */
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
static RTC_DATA_ATTR uint8_t s_config_bytes[EBASE_CONFIG_STORAGE_MAX_BYTES];
#endif

static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
}

static uint8_t *acquire_config_bytes(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    uint8_t *bytes = malloc(EBASE_CONFIG_STORAGE_MAX_BYTES);
    if (bytes) memset(bytes, 0, EBASE_CONFIG_STORAGE_MAX_BYTES);
    return bytes;
#else
    wipe(s_config_bytes, sizeof s_config_bytes);
    return s_config_bytes;
#endif
}

static void release_config_bytes(uint8_t *bytes)
{
    if (!bytes) return;
    wipe(bytes, EBASE_CONFIG_STORAGE_MAX_BYTES);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    free(bytes);
#endif
}

/* The v3 decoder accepts one canonical byte representation per valid config:
 * every header byte is checked, payload lengths are exact, and unused text
 * bytes are zero. Compare fields rather than struct padding after readback. */
static bool same_config(const esp_base_remote_config_t *readback,
                        const esp_base_remote_config_t *candidate,
                        uint32_t revision)
{
    return readback->revision == revision &&
        readback->wifi.configured == candidate->wifi.configured &&
        !memcmp(readback->wifi.ssid, candidate->wifi.ssid, sizeof candidate->wifi.ssid) &&
        !memcmp(readback->wifi.password, candidate->wifi.password, sizeof candidate->wifi.password) &&
        readback->mqtt.configured == candidate->mqtt.configured &&
        readback->mqtt.port == candidate->mqtt.port &&
        !memcmp(readback->mqtt.hostname, candidate->mqtt.hostname, sizeof candidate->mqtt.hostname) &&
        !memcmp(readback->mqtt.username, candidate->mqtt.username, sizeof candidate->mqtt.username) &&
        !memcmp(readback->mqtt.password, candidate->mqtt.password, sizeof candidate->mqtt.password) &&
        !memcmp(readback->mqtt.ca_pem, candidate->mqtt.ca_pem, sizeof candidate->mqtt.ca_pem) &&
        !memcmp(readback->mqtt.management_key, candidate->mqtt.management_key,
                sizeof candidate->mqtt.management_key);
}

bool esp_base_remote_config_with_canonical_bytes(const esp_base_remote_config_t *config,
                                                 esp_base_config_bytes_consumer_t consume,
                                                 void *context)
{
    if (!config || !consume) return false;
    uint8_t *bytes = acquire_config_bytes();
    if (!bytes) return false;
    size_t size = 0;
    const bool encoded = ebase_config_encode(config, bytes, &size);
    const bool consumed = encoded && consume(bytes, size, context);
    release_config_bytes(bytes);
    return consumed;
}

static esp_err_t read_config_bytes(uint8_t *bytes, size_t *size)
{
    wipe(bytes, EBASE_CONFIG_STORAGE_MAX_BYTES);
    esp_err_t error = nvs_flash_init_partition(CONFIG_PARTITION);
    if (error != ESP_OK) return error;
    nvs_handle_t handle;
    error = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    *size = 0;
    error = nvs_get_blob(handle, CONFIG_KEY, NULL, size);
    if (error == ESP_OK && (*size < EBASE_CONFIG_HEADER_BYTES || *size > EBASE_CONFIG_STORAGE_MAX_BYTES))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        size_t received = *size;
        error = nvs_get_blob(handle, CONFIG_KEY, bytes, &received);
        if (error == ESP_OK && received != *size) error = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    return error;
}

static bool hash_config(const uint8_t *bytes, size_t size, uint8_t digest[32])
{
    size_t written = 0;
    /* Startup migration precedes the control task's crypto initialization. */
    return psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &written) == PSA_SUCCESS && written == 32;
}

static esp_err_t retire_legacy_config(uint8_t *bytes, size_t *size)
{
    if (!ebase_config_retire_legacy(bytes, size)) return ESP_ERR_INVALID_STATE;
    uint8_t expected[32] = {0}, actual[32] = {0};
    if (!hash_config(bytes, *size, expected)) return ESP_FAIL;
    const size_t expected_size = *size;
    nvs_handle_t handle;
    esp_err_t result = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, CONFIG_KEY, bytes, *size);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
        /* Once writing was attempted, failure never means the old blob won. */
        if (result != ESP_OK || read_config_bytes(bytes, size) != ESP_OK ||
            *size != expected_size || !hash_config(bytes, *size, actual) ||
            memcmp(actual, expected, sizeof expected)) result = ESP_BASE_CONFIG_UNCERTAIN;
    }
    wipe(expected, sizeof expected);
    wipe(actual, sizeof actual);
    return result;
}

static esp_err_t load_config_with_bytes(esp_base_remote_config_t *config, uint8_t *bytes)
{
    size_t size = 0;
    esp_err_t error = read_config_bytes(bytes, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) { *config = (esp_base_remote_config_t){0}; return ESP_OK; }
    if (error != ESP_OK) return error;
    /* Only historical v3 records with configured FRP need one normalization.
     * The strict legacy decoder checks every field before any write. Keeping
     * v3, its original revision and Wi-Fi/MQTT bytes permits signed rollback
     * firmware to read this same record without any compatibility side key. */
    if (bytes[4] == 3 && (bytes[5] & 4)) {
        error = retire_legacy_config(bytes, &size);
        if (error != ESP_OK) return error;
    }
    return ebase_config_decode(bytes, size, config) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    uint8_t *bytes = acquire_config_bytes();
    if (!bytes) return ESP_ERR_NO_MEM;
    const esp_err_t result = load_config_with_bytes(config, bytes);
    release_config_bytes(bytes);
    return result;
}

esp_err_t esp_base_remote_config_commit_verified(const esp_base_remote_config_t *candidate,
                                                uint32_t expected_revision,
                                                esp_base_remote_config_t *committed,
                                                esp_base_remote_config_t *work)
{
    if (!candidate || !committed || !work || work == candidate || work == committed ||
        !ebase_config_valid(candidate) || candidate->revision != expected_revision)
        return ESP_ERR_INVALID_ARG;
    uint8_t *bytes = acquire_config_bytes();
    esp_err_t result = ESP_ERR_NO_MEM;
    size_t bytes_size = 0;
    nvs_handle_t handle;
    if (!bytes) goto done;
    result = load_config_with_bytes(work, bytes);
    if (result != ESP_OK) goto done;
    if (work->revision != expected_revision) { result = ESP_BASE_CONFIG_CONFLICT; goto done; }
    if (expected_revision == UINT32_MAX) { result = ESP_BASE_CONFIG_EXHAUSTED; goto done; }
    *work = *candidate;
    work->revision = expected_revision + 1;
    if (!ebase_config_encode(work, bytes, &bytes_size)) {
        result = ESP_ERR_INVALID_ARG; goto done;
    }
    result = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) goto done;
    result = nvs_set_blob(handle, CONFIG_KEY, bytes, bytes_size);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) { result = ESP_BASE_CONFIG_UNCERTAIN; goto done; }
    if (load_config_with_bytes(work, bytes) != ESP_OK ||
        !same_config(work, candidate, expected_revision + 1)) {
        result = ESP_BASE_CONFIG_UNCERTAIN; goto done;
    }
    *committed = *work;
done:
    wipe(work, sizeof *work);
    release_config_bytes(bytes);
    return result;
}
