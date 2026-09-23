#include "esp_base_ota.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

#if defined(ESP_PLATFORM) && !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "ESP Base pending-slot confirmation requires bootloader rollback"
#endif

static const char *TAG = "base_ota";

esp_err_t esp_base_ota_inspect(esp_base_ota_t *ota)
{
    if (ota == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota->running_partition = NULL;
    ota->state = ESP_BASE_OTA_STATE_UNKNOWN;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ota->running_partition = running->label;

    esp_ota_img_states_t state;
    const esp_err_t result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_ERR_NOT_FOUND) {
        ota->state = ESP_BASE_OTA_STATE_UNTRACKED;
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    ota->state = state == ESP_OTA_IMG_PENDING_VERIFY ? ESP_BASE_OTA_STATE_PENDING_VERIFY :
                 state == ESP_OTA_IMG_VALID ? ESP_BASE_OTA_STATE_VALID : ESP_BASE_OTA_STATE_OTHER;
    return ESP_OK;
}

const char *esp_base_ota_state_name(esp_base_ota_state_t state)
{
    switch (state) {
    case ESP_BASE_OTA_STATE_UNTRACKED: return "untracked";
    case ESP_BASE_OTA_STATE_PENDING_VERIFY: return "pending_verify";
    case ESP_BASE_OTA_STATE_VALID: return "valid";
    case ESP_BASE_OTA_STATE_OTHER: return "other";
    default: return "unknown";
    }
}

esp_err_t esp_base_ota_reject_pending(esp_base_ota_t *ota)
{
    if (ota == NULL || ota->running_partition == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ota->state != ESP_BASE_OTA_STATE_PENDING_VERIFY) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* A successful SDK call reboots. A returned error may follow a partial
     * otadata write, so observe the durable state before reporting it. */
    (void)esp_base_ota_inspect(ota);
    return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
}

esp_err_t esp_base_ota_confirm_if_stable(esp_base_ota_t *ota,
                                         uint64_t stable_started_ms, uint64_t now_ms)
{
    if (ota == NULL || ota->running_partition == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ota->state == ESP_BASE_OTA_STATE_VALID || ota->state == ESP_BASE_OTA_STATE_UNTRACKED) {
        return ESP_OK;
    }
    if (ota->state != ESP_BASE_OTA_STATE_PENDING_VERIFY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (now_ms < stable_started_ms) {
        return ESP_ERR_INVALID_STATE;
    }
    if (now_ms - stable_started_ms < ESP_BASE_OTA_STABLE_WINDOW_MS) {
        return ESP_ERR_NOT_FINISHED;
    }
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    const esp_err_t inspect_result = esp_base_ota_inspect(ota);
    /* A write may have reached otadata even when the SDK reports failure.
     * The durable VALID state is the only successful confirmation fact. */
    if (inspect_result == ESP_OK && ota->state == ESP_BASE_OTA_STATE_VALID) {
        ESP_LOGI(TAG, "local self-test and stability window passed; slot %s is valid", ota->running_partition);
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    if (inspect_result != ESP_OK) {
        return inspect_result;
    }
    return ESP_ERR_INVALID_STATE;
}
