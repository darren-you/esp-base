#include "esp_base_remote_config.h"

#include <stddef.h>

#include "nvs.h"

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open("base_config", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        config->generation = 0;
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_get_u32(handle, "generation", &config->generation);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        config->generation = 0;
        return ESP_OK;
    }
    return result;
}
