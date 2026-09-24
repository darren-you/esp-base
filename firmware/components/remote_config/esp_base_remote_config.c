// SPDX-License-Identifier: Apache-2.0
#include "esp_base_remote_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_PARTITION "base_store"
#define CONFIG_NAMESPACE "base_config"
#define CONFIG_KEY "committed"

/* The config can approach 5 KiB. These buffers are owned by the startup and
 * subsequently single USB control task; never place them on its 6 KiB stack. */
/* Loading and commit verification run under the same Base control owner.
 * A commit reuses the load buffer only after its readback load has returned
 * and wiped it; the encoded candidate stays in the separate commit buffer. */
static uint8_t s_load_bytes[EBASE_CONFIG_MAX_BYTES];
static uint8_t s_commit_bytes[EBASE_CONFIG_MAX_BYTES];
static esp_base_remote_config_t s_work;

static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
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
        error = nvs_get_blob(handle, CONFIG_KEY, s_load_bytes, &received);
        if (error == ESP_OK && received != size) error = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { *config = (esp_base_remote_config_t){0}; return ESP_OK; }
    if (error != ESP_OK) {
        wipe(s_load_bytes, sizeof s_load_bytes);
        return error;
    }
    const bool valid = ebase_config_decode(s_load_bytes, size, config);
    wipe(s_load_bytes, sizeof s_load_bytes);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_base_remote_config_commit_verified(const esp_base_remote_config_t *candidate,
                                                uint32_t expected_revision,
                                                esp_base_remote_config_t *committed)
{
    if (!candidate || !committed || !ebase_config_valid(candidate) || candidate->revision != expected_revision)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = esp_base_remote_config_load(&s_work);
    size_t bytes_size = 0, readback_size = 0;
    nvs_handle_t handle;
    if (result != ESP_OK) goto done;
    if (s_work.revision != expected_revision) { result = ESP_BASE_CONFIG_CONFLICT; goto done; }
    if (expected_revision == UINT32_MAX) { result = ESP_BASE_CONFIG_EXHAUSTED; goto done; }
    s_work = *candidate;
    s_work.revision = expected_revision + 1;
    if (!ebase_config_encode(&s_work, s_commit_bytes, &bytes_size)) {
        result = ESP_ERR_INVALID_ARG; goto done;
    }
    result = nvs_open_from_partition(CONFIG_PARTITION, CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) goto done;
    result = nvs_set_blob(handle, CONFIG_KEY, s_commit_bytes, bytes_size);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) { result = ESP_BASE_CONFIG_UNCERTAIN; goto done; }
    if (esp_base_remote_config_load(&s_work) != ESP_OK ||
        !ebase_config_encode(&s_work, s_load_bytes, &readback_size) ||
        bytes_size != readback_size || memcmp(s_commit_bytes, s_load_bytes, bytes_size)) {
        result = ESP_BASE_CONFIG_UNCERTAIN; goto done;
    }
    *committed = s_work;
done:
    wipe(&s_work, sizeof s_work);
    wipe(s_commit_bytes, sizeof s_commit_bytes);
    wipe(s_load_bytes, sizeof s_load_bytes);
    return result;
}
