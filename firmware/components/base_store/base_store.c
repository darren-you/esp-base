// SPDX-License-Identifier: Apache-2.0
#include "base_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NAMESPACE "base"

esp_err_t base_store_init(void)
{
    /* 不在出错时擦除：分区里有出厂身份与配置。 */
    return nvs_flash_init();
}

static esp_err_t get_string(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    size_t length = size;
    const esp_err_t result = nvs_get_str(handle, key, out, &length);
    if (result != ESP_OK) out[0] = '\0';
    return result;
}

esp_err_t base_store_device_id(char out[BASE_UUID_BYTES])
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = get_string(handle, "device_id", out, BASE_UUID_BYTES);
    if (result == ESP_OK && !base_is_uuid(out)) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t b[16];
        esp_fill_random(b, sizeof b);
        b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
        b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
        snprintf(out, BASE_UUID_BYTES, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        result = nvs_set_str(handle, "device_id", out);
        if (result == ESP_OK) result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t base_store_load_config(base_config_t *config)
{
    memset(config, 0, sizeof *config);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    result = get_string(handle, "wifi_ssid", config->wifi_ssid, sizeof config->wifi_ssid);
    if (result == ESP_OK) result = get_string(handle, "wifi_pass", config->wifi_password, sizeof config->wifi_password);
    if (result == ESP_OK) result = get_string(handle, "mqtt_uri", config->mqtt_uri, sizeof config->mqtt_uri);
    if (result == ESP_OK) result = get_string(handle, "mqtt_user", config->mqtt_username, sizeof config->mqtt_username);
    if (result == ESP_OK) result = get_string(handle, "mqtt_pass", config->mqtt_password, sizeof config->mqtt_password);
    if (result == ESP_OK && nvs_get_u32(handle, "cfg_rev", &config->revision) != ESP_OK) config->revision = 0;
    nvs_close(handle);
    return result;
}

esp_err_t base_store_save_config(base_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_str(handle, "wifi_ssid", config->wifi_ssid);
    if (result == ESP_OK) result = nvs_set_str(handle, "wifi_pass", config->wifi_password);
    if (result == ESP_OK) result = nvs_set_str(handle, "mqtt_uri", config->mqtt_uri);
    if (result == ESP_OK) result = nvs_set_str(handle, "mqtt_user", config->mqtt_username);
    if (result == ESP_OK) result = nvs_set_str(handle, "mqtt_pass", config->mqtt_password);
    if (result == ESP_OK) result = nvs_set_u32(handle, "cfg_rev", config->revision + 1);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (result == ESP_OK) ++config->revision;
    nvs_close(handle);
    return result;
}

esp_err_t base_store_load_ota(base_ota_record_t *record)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    size_t length = sizeof *record;
    result = nvs_get_blob(handle, "ota", record, &length);
    if (result == ESP_OK && length != sizeof *record) result = ESP_ERR_INVALID_SIZE;
    nvs_close(handle);
    return result;
}

esp_err_t base_store_save_ota(const base_ota_record_t *record)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "ota", record, sizeof *record);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
