#pragma once

#include "esp_base_storage_owner.h"
#include "esp_base_ota_receipt.h"
#include "eota.h"
#include <stddef.h>

typedef enum {
    ESP_BASE_CONTAINER_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_BLOCKED,
    ESP_BASE_CONTAINER_EMPTY,
    ESP_BASE_CONTAINER_RUNNING,
    ESP_BASE_CONTAINER_STOPPED,
} esp_base_container_boot_result_t;
esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);
typedef enum {
    ESP_BASE_CONTAINER_UNINSTALL_COMPLETE = 0,
    ESP_BASE_CONTAINER_UNINSTALL_NOT_CONFIGURED,
    ESP_BASE_CONTAINER_UNINSTALL_REJECTED,
    ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN,
} esp_base_container_uninstall_result_t;
esp_base_container_uninstall_result_t esp_base_container_product_uninstall(
    const esp_base_storage_claim_t *claim, const char operation_id[37],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32]);
typedef enum {
    ESP_BASE_CONTAINER_UNINSTALL_RECOVERED = 0,
    ESP_BASE_CONTAINER_UNINSTALL_NOT_COMMITTED,
    ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN,
} esp_base_container_uninstall_recovery_t;
esp_base_container_uninstall_recovery_t esp_base_container_product_reconcile_uninstall(
    const esp_base_storage_claim_t *claim, const char operation_id[37],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32]);
typedef enum {
    ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN = 0,
    ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED,
    ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED,
} esp_base_container_package_recovery_t;
esp_base_container_package_recovery_t esp_base_container_product_recover_pending_package(
    const esp_base_storage_claim_t *claim, const char boot_id[37],
    const char operation_id[37], uint32_t expected_sequence,
    const uint8_t package_sha256[32], uint32_t *resolved_sequence);

typedef enum {
    ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_STAGE_PREPARED,
    ESP_BASE_CONTAINER_STAGE_WRITING,
    ESP_BASE_CONTAINER_STAGE_REJECTED,
    ESP_BASE_CONTAINER_STAGE_UNCERTAIN,
} esp_base_container_stage_result_t;

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const esp_base_ota_receipt_recovery_t *receipt);
bool esp_base_container_product_ota_ready(esp_base_ota_package_mode_t package_mode);
bool esp_base_container_product_configured(void);
bool esp_base_container_product_pristine_baseline(
    const esp_base_storage_claim_t *claim);
typedef struct {
    uint32_t container_sequence;
    uint8_t firmware_sha256[32];
    uint32_t runtime_guest_abi_version;
    bool package_present;
    uint8_t package_sha256[32];
    uint32_t package_guest_abi_version;
    uint32_t package_data_schema_version;
} esp_base_container_binding_snapshot_t;
typedef enum {
    ESP_BASE_CONTAINER_BINDING_OK = 0,
    ESP_BASE_CONTAINER_BINDING_NOT_CONFIGURED,
    ESP_BASE_CONTAINER_BINDING_BUSY,
    ESP_BASE_CONTAINER_BINDING_UNCERTAIN,
    ESP_BASE_CONTAINER_BINDING_RESOURCE_FAILURE,
} esp_base_container_binding_result_t;
esp_base_container_binding_result_t esp_base_container_product_binding_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *out);
typedef bool (*econtainer_slot_source_fn)(void *context, size_t offset_bytes,
                                           uint8_t *destination, size_t size_bytes);
esp_base_container_stage_result_t esp_base_container_product_write_staged_firmware_package(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const esp_base_ota_receipt_recovery_t *receipt,
    econtainer_slot_source_fn source_fn, void *source_context);
typedef struct {
    bool present;
    const char *product_id; /* Immutable build authorization, verified on open. */
    char *product_version; /* Owned copy; caller frees after a successful query. */
    size_t product_version_size_bytes;
    uint8_t package_sha256[32];
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
    bool is_trial;
    char operation_id[37]; /* Empty unless is_trial. */
} esp_base_container_active_product_t;

esp_base_container_binding_result_t esp_base_container_product_status_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *binding,
    esp_base_container_active_product_t *active);

typedef struct {
    char operation_id[37];
    uint32_t expected_sequence;
    bool previous_package_present;
    uint8_t previous_package_sha256[32];
    uint8_t package_sha256[32];
    uint32_t package_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
} esp_base_container_package_request_t;
typedef enum {
    ESP_BASE_CONTAINER_PREPARED = 0,
    ESP_BASE_CONTAINER_PREPARE_REJECTED,
    ESP_BASE_CONTAINER_PREPARE_BUSY,
    ESP_BASE_CONTAINER_PREPARE_UNCERTAIN,
} esp_base_container_prepare_result_t;
esp_base_container_prepare_result_t esp_base_container_product_prepare_package(
    const esp_base_storage_claim_t *claim,
    const esp_base_container_package_request_t *request,
    econtainer_slot_source_fn source_fn, void *source_context,
    uint32_t *prepared_sequence);
bool esp_base_container_product_abandon_prepared_package(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[37], const uint8_t package_sha256[32],
    uint32_t *aborted_sequence);
bool esp_base_container_product_stop_confirmed(
    const esp_base_storage_claim_t *claim);
esp_base_container_boot_result_t esp_base_container_product_start_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[37], const char boot_id[37],
    const uint8_t trial_event_sha256[32]);
bool esp_base_container_product_abandon_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[37]);
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
    const uint8_t event_sha256[32], const uint8_t *event, size_t size_bytes);
bool esp_base_container_product_event_accepting(void);
bool esp_base_container_product_trial_quiescent(void);
typedef enum {
    ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED,
    ESP_BASE_CONTAINER_CONFIRM_CONFIRMED,
    ESP_BASE_CONTAINER_CONFIRM_UNCERTAIN,
} esp_base_container_trial_confirm_result_t;
esp_base_container_trial_confirm_result_t esp_base_container_product_confirm_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[37], uint64_t verified_event_sequence,
    const uint8_t verified_event_sha256[32], uint64_t verified_failure_count,
    uint32_t *confirmed_sequence);
typedef struct {
    uint8_t package_sha256[32];
    uint8_t event_sha256[32];
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
typedef struct {
    uint64_t representative_event_sequence;
    uint64_t failure_count;
    uint8_t package_sha256[32];
} esp_base_container_trial_event_snapshot_t;
bool esp_base_container_product_trial_event_snapshot(
    esp_base_container_trial_event_snapshot_t *out);
bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim, const esp_base_ota_request_t *request,
    esp_base_ota_receipt_snapshot_t *snapshot);
typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;
esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt);
