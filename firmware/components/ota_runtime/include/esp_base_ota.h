#pragma once

#include <stdint.h>

#include "esp_err.h"

#define ESP_BASE_OTA_STABLE_WINDOW_MS UINT64_C(30000)

typedef enum {
    ESP_BASE_OTA_STATE_UNKNOWN,
    ESP_BASE_OTA_STATE_UNTRACKED,
    ESP_BASE_OTA_STATE_PENDING_VERIFY,
    ESP_BASE_OTA_STATE_VALID,
    ESP_BASE_OTA_STATE_OTHER,
} esp_base_ota_state_t;

typedef struct {
    const char *running_partition;
    esp_base_ota_state_t state;
} esp_base_ota_t;

esp_err_t esp_base_ota_inspect(esp_base_ota_t *ota);
const char *esp_base_ota_state_name(esp_base_ota_state_t state);
/* On success the IDF API reboots and does not return. Without a valid previous
 * image it returns ESP_ERR_OTA_ROLLBACK_FAILED and leaves this boot running. */
esp_err_t esp_base_ota_reject_pending(esp_base_ota_t *ota);
/* Call only after local startup checks have succeeded.
 * ESP_ERR_NOT_FINISHED means this boot has not survived the full window.
 * The observed state is refreshed even if the SDK confirmation fails. */
esp_err_t esp_base_ota_confirm_if_stable(esp_base_ota_t *ota,
                                         uint64_t stable_started_ms, uint64_t now_ms);
