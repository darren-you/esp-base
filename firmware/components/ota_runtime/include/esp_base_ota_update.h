#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESP_BASE_OTA_URL_BYTES 512
#define ESP_BASE_OTA_OPERATION_ID_BYTES 37
#define ESP_BASE_OTA_TARGET "esp32c3/esp_base"
#define ESP_BASE_OTA_SIGNATURE_SCHEME "esp_secure_boot_v2_rsa3072"

typedef struct {
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    char image_url[ESP_BASE_OTA_URL_BYTES + 1];
    uint8_t sha256[32];
    uint32_t image_size_bytes;
} esp_base_ota_update_request_t;

typedef enum {
    ESP_BASE_OTA_UPDATE_OK,
    ESP_BASE_OTA_UPDATE_UNSUPPORTED,
    ESP_BASE_OTA_UPDATE_INVALID_REQUEST,
    ESP_BASE_OTA_UPDATE_SLOT_UNAVAILABLE,
    ESP_BASE_OTA_UPDATE_TOO_LARGE,
    ESP_BASE_OTA_UPDATE_WRONG_TARGET,
    ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED,
    ESP_BASE_OTA_UPDATE_HASH_MISMATCH,
    ESP_BASE_OTA_UPDATE_SIGNATURE_INVALID,
    ESP_BASE_OTA_UPDATE_BOOT_STATE_UNKNOWN,
    ESP_BASE_OTA_UPDATE_RESOURCE_FAILURE,
} esp_base_ota_update_result_t;

typedef void (*esp_base_ota_progress_t)(uint32_t received_bytes, uint32_t total_bytes, void *context);

/* Signed software OTA uses the public key of the currently running signed app.
 * This is false for the ordinary, unsigned Base image. */
bool esp_base_ota_update_available(void);
esp_base_ota_update_result_t esp_base_ota_update_run(
    const esp_base_ota_update_request_t *request, esp_base_ota_progress_t progress, void *context);
const char *esp_base_ota_update_error(esp_base_ota_update_result_t result);
