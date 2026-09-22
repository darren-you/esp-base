// SPDX-License-Identifier: Apache-2.0
#include "esp_base_remote_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_PARTITION "base_store"
#define CONFIG_NAMESPACE "base_config"
#define CONFIG_KEY "committed"

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    esp_err_t error = nvs_flash_init_partition(CONFIG_PARTITION);
    if (error != ESP_OK) return error;
    nvs_handle_t handle;
    error = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { *config = (esp_base_remote_config_t){0}; return ESP_OK; }
    if (error != ESP_OK) return error;
    uint8_t bytes[EBASE_CONFIG_BYTES];
    size_t size = sizeof bytes;
    error = nvs_get_blob(handle, CONFIG_KEY, bytes, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { *config = (esp_base_remote_config_t){0}; return ESP_OK; }
    if (error != ESP_OK) return error;
    return ebase_config_decode(bytes, size, config) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_base_remote_config_commit_verified(const esp_base_remote_config_t *candidate,
                                                uint32_t expected_revision,
                                                esp_base_remote_config_t *committed)
{
    if (!candidate || !committed || !ebase_config_valid(candidate) || candidate->revision != expected_revision)
        return ESP_ERR_INVALID_ARG;
    esp_base_remote_config_t previous;
    esp_err_t error = esp_base_remote_config_load(&previous);
    if (error != ESP_OK) return error;
    if (previous.revision != expected_revision) return ESP_BASE_CONFIG_CONFLICT;
    if (expected_revision == UINT32_MAX) return ESP_BASE_CONFIG_EXHAUSTED;
    esp_base_remote_config_t next = *candidate;
    next.revision = expected_revision + 1;
    uint8_t bytes[EBASE_CONFIG_BYTES], actual[EBASE_CONFIG_BYTES];
    if (!ebase_config_encode(&next, bytes)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    error = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, CONFIG_KEY, bytes, sizeof bytes);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return ESP_BASE_CONFIG_UNCERTAIN;
    esp_base_remote_config_t observed;
    if (esp_base_remote_config_load(&observed) != ESP_OK || !ebase_config_encode(&observed, actual) ||
        memcmp(bytes, actual, sizeof bytes)) return ESP_BASE_CONFIG_UNCERTAIN;
    *committed = observed;
    return ESP_OK;
}
