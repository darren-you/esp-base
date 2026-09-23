#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_base_ota_update.h"

typedef enum {
    ESP_BASE_OTA_RECEIPT_OK,
    ESP_BASE_OTA_RECEIPT_UNSUPPORTED,
    ESP_BASE_OTA_RECEIPT_NOT_FOUND,
    ESP_BASE_OTA_RECEIPT_EXISTS,
    ESP_BASE_OTA_RECEIPT_CONFLICT,
    ESP_BASE_OTA_RECEIPT_BUSY,
    ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE,
    ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH,
    ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID,
    ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE,
    ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN,
    ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE,
    ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN,
} esp_base_ota_receipt_result_t;

typedef enum {
    ESP_BASE_OTA_OPERATION_UNKNOWN,
    ESP_BASE_OTA_OPERATION_RUNNING,
    ESP_BASE_OTA_OPERATION_SUCCEEDED,
    ESP_BASE_OTA_OPERATION_FAILED,
} esp_base_ota_operation_state_t;

typedef struct {
    esp_base_ota_operation_state_t state;
    const char *error_code;
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint8_t sha256[32];
    uint32_t image_size_bytes;
    uint8_t target_subtype;
} esp_base_ota_receipt_view_t;

/* One latest operation is retained in base_store/base_ota/operation. A new
 * operation may replace only a terminal result; the same ID never downloads
 * twice. Commit and exact readback precede the first target-slot write. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_update_request_t *request);
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, esp_base_ota_update_result_t error);
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view);
