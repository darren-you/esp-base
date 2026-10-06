#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_base_ota_policy.h"

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
    ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH,
    ESP_BASE_OTA_RECEIPT_SAME_IMAGE,
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

/* Captured under the app/otadata owner immediately before intent registration.
 * Identities are the verified distinct signed Base firmware set; a zero inactive
 * digest denotes an A-only set. */
typedef struct esp_base_ota_receipt_snapshot {
    uint8_t source_sha256[EOTA_SHA256_BYTES];
    uint8_t inactive_sha256[EOTA_SHA256_BYTES];
} esp_base_ota_receipt_snapshot_t;

typedef enum {
    ESP_BASE_OTA_RECEIPT_PREPARED = 1,
    ESP_BASE_OTA_RECEIPT_FAILED = 2,
    ESP_BASE_OTA_RECEIPT_SUCCEEDED = 3,
} esp_base_ota_receipt_status_t;

typedef struct {
    esp_base_ota_receipt_status_t status;
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint8_t source_subtype;
    uint8_t target_subtype;
    uint8_t source_sha256[EOTA_SHA256_BYTES];
    uint8_t inactive_sha256[EOTA_SHA256_BYTES];
    uint8_t candidate_sha256[EOTA_SHA256_BYTES];
    uint32_t image_size_bytes;
} esp_base_ota_receipt_recovery_t;

/* V4 retains one firmware-only operation in base_store/base_ota/operation.
 * Commit and byte-for-byte readback precede target retirement. New operations
 * replace only proven terminal results; the same ID never starts twice.
 * Old V1/V2/V3 and corrupt records are uncertain, never absent. Their migration
 * must be resolved explicitly in the first wired layout assembly. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_request_t *request,
    const esp_base_ota_receipt_snapshot_t *snapshot);
/* Load the exact V4 intent. Reading never authorizes erasure or advancement;
 * the caller must prove the receipt-bound physical state. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *recovery);
/* Record failure after no app mutation or complete receipt-bound physical
 * reconciliation. The caller holds the upgrade owner throughout. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error);
/* The caller proves local initialization and OTA VALID. This independently
 * verifies the signed running candidate identity before durable success. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_success(
    const char *device_id);
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view);
