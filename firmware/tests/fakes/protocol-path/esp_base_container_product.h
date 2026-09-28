#pragma once

#include "esp_base_storage_owner.h"
#include "esp_base_ota_receipt.h"
#include "eota.h"

typedef enum {
    ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_STAGE_PREPARED,
    ESP_BASE_CONTAINER_STAGE_REJECTED,
    ESP_BASE_CONTAINER_STAGE_UNCERTAIN,
} esp_base_container_stage_result_t;

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[37]);
bool esp_base_container_product_ota_ready(void);
bool esp_base_container_product_configured(void);
bool esp_base_container_product_pristine_baseline(
    const esp_base_storage_claim_t *claim);
typedef struct {
    uint32_t container_sequence;
    bool package_present;
    uint8_t package_sha256[32];
} esp_base_container_binding_snapshot_t;
typedef enum {
    ESP_BASE_CONTAINER_BINDING_OK = 0,
    ESP_BASE_CONTAINER_BINDING_NOT_CONFIGURED,
    ESP_BASE_CONTAINER_BINDING_BUSY,
    ESP_BASE_CONTAINER_BINDING_UNCERTAIN,
} esp_base_container_binding_result_t;
esp_base_container_binding_result_t esp_base_container_product_binding_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *out);
typedef enum {
    ESP_BASE_CONTAINER_EVENT_ACCEPTED = 0,
    ESP_BASE_CONTAINER_EVENT_UNAVAILABLE,
    ESP_BASE_CONTAINER_EVENT_INVALID,
    ESP_BASE_CONTAINER_EVENT_FULL,
    ESP_BASE_CONTAINER_EVENT_NO_MEMORY,
    ESP_BASE_CONTAINER_EVENT_BUSY,
} esp_base_container_event_result_t;
esp_base_container_event_result_t esp_base_container_product_offer_event(
    const uint8_t package_sha256[32], uint64_t event_sequence,
    const uint8_t *event, size_t size_bytes);
typedef struct {
    uint8_t package_sha256[32];
    uint64_t event_sequence;
    int32_t guest_result;
    bool runtime_ok;
} esp_base_container_event_observation_t;
typedef enum {
    ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION = 0,
    ESP_BASE_CONTAINER_EVENT_OBSERVED,
    ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY,
} esp_base_container_event_observation_result_t;
esp_base_container_event_observation_result_t
esp_base_container_product_event_observation(
    esp_base_container_event_observation_t *out);
bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_receipt_snapshot_t *snapshot);
typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;
esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32]);
