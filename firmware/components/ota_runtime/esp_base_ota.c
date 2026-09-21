#include "esp_base_ota.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "base_ota";

esp_err_t esp_base_ota_inspect(esp_base_ota_t *ota)
{
    if (ota == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ota->running_partition = running->label;

    esp_ota_img_states_t state;
    const esp_err_t result = esp_ota_get_state_partition(running, &state);
    ota->pending_verify = result == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    return result;
}

esp_err_t esp_base_ota_mark_valid_if_pending(const esp_base_ota_t *ota)
{
    if (ota == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota->pending_verify) {
        ESP_LOGI(TAG, "running slot %s does not require validation", ota->running_partition);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "local self-test passed; marking slot %s valid", ota->running_partition);
    return esp_ota_mark_app_valid_cancel_rollback();
}
