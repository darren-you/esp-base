// SPDX-License-Identifier: Apache-2.0
#include "esp_base_remote_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_PARTITION "base_store"
#define CONFIG_NAMESPACE "base_config"
#define CONFIG_KEY "committed"

/* Startup and the single control task own this buffer. NVS copies the encoded
 * candidate before readback reuses it; neither operation runs concurrently. */
static uint8_t s_config_bytes[EBASE_CONFIG_MAX_BYTES];

static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
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
                sizeof candidate->mqtt.management_key) &&
        readback->frp.configured == candidate->frp.configured &&
        readback->frp.server_port == candidate->frp.server_port &&
        readback->frp.remote_port == candidate->frp.remote_port &&
        readback->frp.local_port == candidate->frp.local_port &&
        !memcmp(readback->frp.server_hostname, candidate->frp.server_hostname,
                sizeof candidate->frp.server_hostname) &&
        !memcmp(readback->frp.token, candidate->frp.token, sizeof candidate->frp.token) &&
        !memcmp(readback->frp.ca_pem, candidate->frp.ca_pem, sizeof candidate->frp.ca_pem) &&
        !memcmp(readback->frp.proxy_name, candidate->frp.proxy_name,
                sizeof candidate->frp.proxy_name) &&
        !memcmp(readback->frp.management_key, candidate->frp.management_key,
                sizeof candidate->frp.management_key);
}

bool esp_base_remote_config_with_canonical_bytes(const esp_base_remote_config_t *config,
                                                 esp_base_config_bytes_consumer_t consume,
                                                 void *context)
{
    if (!config || !consume) return false;
    size_t size = 0;
    const bool encoded = ebase_config_encode(config, s_config_bytes, &size);
    const bool consumed = encoded && consume(s_config_bytes, size, context);
    wipe(s_config_bytes, sizeof s_config_bytes);
    return consumed;
}

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    esp_err_t error = nvs_flash_init_partition(CONFIG_PARTITION);
    if (error != ESP_OK) return error;
    nvs_handle_t handle;
    error = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { *config = (esp_base_remote_config_t){0}; return ESP_OK; }
    if (error != ESP_OK) return error;
    size_t size = 0;
    error = nvs_get_blob(handle, CONFIG_KEY, NULL, &size);
    if (error == ESP_OK && (size < EBASE_CONFIG_HEADER_BYTES || size > EBASE_CONFIG_MAX_BYTES))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        size_t received = size;
        error = nvs_get_blob(handle, CONFIG_KEY, s_config_bytes, &received);
        if (error == ESP_OK && received != size) error = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { *config = (esp_base_remote_config_t){0}; return ESP_OK; }
    if (error != ESP_OK) {
        wipe(s_config_bytes, sizeof s_config_bytes);
        return error;
    }
    const bool valid = ebase_config_decode(s_config_bytes, size, config);
    wipe(s_config_bytes, sizeof s_config_bytes);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_base_remote_config_commit_verified(const esp_base_remote_config_t *candidate,
                                                uint32_t expected_revision,
                                                esp_base_remote_config_t *committed,
                                                esp_base_remote_config_t *work)
{
    if (!candidate || !committed || !work || work == candidate || work == committed ||
        !ebase_config_valid(candidate) || candidate->revision != expected_revision)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = esp_base_remote_config_load(work);
    size_t bytes_size = 0;
    nvs_handle_t handle;
    if (result != ESP_OK) goto done;
    if (work->revision != expected_revision) { result = ESP_BASE_CONFIG_CONFLICT; goto done; }
    if (expected_revision == UINT32_MAX) { result = ESP_BASE_CONFIG_EXHAUSTED; goto done; }
    *work = *candidate;
    work->revision = expected_revision + 1;
    if (!ebase_config_encode(work, s_config_bytes, &bytes_size)) {
        result = ESP_ERR_INVALID_ARG; goto done;
    }
    result = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) goto done;
    result = nvs_set_blob(handle, CONFIG_KEY, s_config_bytes, bytes_size);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) { result = ESP_BASE_CONFIG_UNCERTAIN; goto done; }
    if (esp_base_remote_config_load(work) != ESP_OK ||
        !same_config(work, candidate, expected_revision + 1)) {
        result = ESP_BASE_CONFIG_UNCERTAIN; goto done;
    }
    *committed = *work;
done:
    wipe(work, sizeof *work);
    wipe(s_config_bytes, sizeof s_config_bytes);
    return result;
}
