// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#include "esp_base_storage_owner.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
#include "eota.h"

typedef enum {
    ESP_BASE_CONTAINER_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_BLOCKED,
    ESP_BASE_CONTAINER_EMPTY,
    ESP_BASE_CONTAINER_RUNNING,
    ESP_BASE_CONTAINER_STOPPED,
} esp_base_container_boot_result_t;

/* Bind the boot's short Flash/NVS I/O owner before any product storage work.
 * It is separate from the long app/otadata and product transaction owner. */
void esp_base_container_product_set_flash_io_owner(esp_base_storage_owner_t *owner);

/* Called with Base's already active boot claim after NVS and control start.
 * A fully specified product policy binds exact real package/NVS partitions,
 * then opens and initializes a persisted confirmed package on one pthread.
 * EMPTY/RUNNING have finished storage admission and the caller releases its
 * claim. BLOCKED retains the claim because storage state may be uncertain.
 * The running guest never owns this app/otadata operation claim. */
esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);

/* PENDING_VERIFY starts only the exact PREPARED firmware transition and uses
 * this boot's protocol UUID. A no-package trial still advances durable state. */
esp_base_container_boot_result_t esp_base_container_product_start_trial(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);
bool esp_base_container_product_mark_healthy(const esp_base_storage_claim_t *claim);
bool esp_base_container_product_confirm_firmware(const esp_base_storage_claim_t *claim);
/* Synchronize with the unique guest pthread before rejecting a pending app.
 * False means native reclamation or guest stop was not proven: retain owner
 * and do not ask IDF to roll back while candidate code may still execute. */
bool esp_base_container_product_stop_trial(const esp_base_storage_claim_t *claim);

/* Product-only changes use the same Base storage claim as firmware OTA. Stop
 * and join the confirmed guest's unique executor before changing its binding.
 * Success proves native reclamation and permits product_boot to reopen the
 * persisted confirmed binding in this boot. Failure retains the claim. */
bool esp_base_container_product_stop_confirmed(
    const esp_base_storage_claim_t *claim);

/* After an EMPTY boot, prove the durable ECS2 record is exactly its initial
 * no-package binding for the currently signed firmware set. A later history,
 * changed firmware identity or unreadable state forbids first ledger creation. */
bool esp_base_container_product_pristine_baseline(
    const esp_base_storage_claim_t *claim);

typedef enum {
    ESP_BASE_CONTAINER_EVENT_ACCEPTED = 0,
    ESP_BASE_CONTAINER_EVENT_UNAVAILABLE,
    ESP_BASE_CONTAINER_EVENT_INVALID,
    ESP_BASE_CONTAINER_EVENT_FULL,
    ESP_BASE_CONTAINER_EVENT_NO_MEMORY,
    ESP_BASE_CONTAINER_EVENT_BUSY,
} esp_base_container_event_result_t;

/* Only an authenticated Base ingress may call this. Copy one event into the
 * signed package's bounded FIFO; the MQTT/control task never executes guest
 * code. The exact package digest prevents delivery across a same-boot switch.
 * ACCEPTED means queued, not processed or successful. */
esp_base_container_event_result_t esp_base_container_product_offer_event(
    const uint8_t package_sha256[32], const uint8_t *event, size_t size_bytes);
bool esp_base_container_product_event_accepting(void);
/* Counts completed guest calls with runtime OK. The caller must still check
 * the product-specific result; this counter alone is not a trial health proof. */
uint32_t esp_base_container_product_event_progress_count(void);

typedef enum {
    ESP_BASE_CONTAINER_UNINSTALL_COMPLETE = 0,
    ESP_BASE_CONTAINER_UNINSTALL_NOT_CONFIGURED,
    ESP_BASE_CONTAINER_UNINSTALL_REJECTED,
    ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN,
} esp_base_container_uninstall_result_t;

/* Internal product operation boundary; no device command is exposed. The
 * caller holds the same Base claim used by OTA, supplies the exact persisted
 * sequence and current package digest, and has resolved the selected firmware
 * OTA receipt. A RUNNING confirmed guest is stopped, closed and joined;
 * STOPPED or BLOCKED guests need a joined worker and proven native cleanup.
 * Container then clears only the running-firmware binding. COMPLETE includes an
 * independent durable readback and permits same-boot product_boot to observe
 * EMPTY. Any stop, commit, readback or firmware-observation uncertainty keeps
 * same-boot reopening blocked and requires durable-state recovery. */
esp_base_container_uninstall_result_t esp_base_container_product_uninstall(
    const esp_base_storage_claim_t *claim,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32]);

/* A complete product policy is required for the persistent binding path.
 * NO_PACKAGE can enter firmware trial; package trials need a real authorized
 * business event source and are rejected before inactive-app writing. */
bool esp_base_container_product_configured(void);
/* Reject a blocked or uninitialized product before any inactive-app write. */
bool esp_base_container_product_ota_ready(void);
/* Before normal boot without an active V2 receipt, read the real ECS2 key
 * under Base's claim. A genuinely absent key permits first initialization;
 * any persisted firmware transition requires its original receipt. */
bool esp_base_container_product_without_ota_receipt(
    const esp_base_storage_claim_t *claim);

/* The caller holds Base's app/otadata claim. A CONFIRMED double observation
 * supplies exact signed source/inactive digests. With a product policy, the
 * existing ECS2 blob must reconcile, the running binding must have no package,
 * and its sequence is copied into the same durable OTA receipt before erase.
 * No policy still supplies the physical hashes with container_enabled=false. */
bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_receipt_snapshot_t *snapshot);

typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;

/* Called only after eota_retire_inactive has physically erased and read back
 * the exact inactive app under this claim. It accepts the receipt's A/B -> A
 * sequence or an already durable A-only state; PREPARED is never consumed on
 * the OTA worker's same boot. A mismatch retains the claim. */
esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32]);

/* Fresh-boot recovery after physical eota_retire_inactive, before product_boot
 * creates any guest thread. In addition to A/B and A-only it recognizes only
 * the same receipt's exact A/C operation, abandons its candidate, then drops
 * the unbootable C binding. The current boot ID must differ from any recorded
 * trial boot ID; no live guest may be silently canceled. */
esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32], const uint8_t candidate_sha256[32],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37]);

/* Before starting a guest on selected C, bind the original V2 receipt to the
 * exact A/C ECS2 transition and sequence. A successful prior receipt requires
 * a durable CONFIRMED phase; a PREPARED receipt permits the exact pending
 * trial, or completes HEALTH_VERIFIED -> CONFIRMED for a VALID C and reads it
 * back. No normal boot path changes a transition without this receipt. */
bool esp_base_container_product_reconcile_selected_ota(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt, eota_state_t running_state);

typedef enum {
    ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_STAGE_PREPARED,
    ESP_BASE_CONTAINER_STAGE_REJECTED,
    ESP_BASE_CONTAINER_STAGE_UNCERTAIN,
} esp_base_container_stage_result_t;

/* Called by the OTA worker with its existing claim, after eota_prepare and
 * before eota_select. Only a real persisted running binding without a package
 * stages NO_PACKAGE. A package trial lacks an authorized business event source;
 * until that input exists, REUSE and WRITE are rejected before app Flash write. */
esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES]);
