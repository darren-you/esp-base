// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_base_storage_owner.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
#include "esp_container_slots.h"
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
/* Joint firmware/package trial requires the original PREPARED V3 receipt.
 * Reconcile exact pending C, persist its trial boot, then open/init the package.
 * Base must separately establish authorized event and continuous online health. */
esp_base_container_boot_result_t esp_base_container_product_start_firmware_package_trial(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt, const char boot_id[37]);

typedef enum {
    ESP_BASE_CONTAINER_HEALTH_NOT_STARTED,
    ESP_BASE_CONTAINER_HEALTH_VERIFIED,
    ESP_BASE_CONTAINER_HEALTH_UNCERTAIN,
} esp_base_container_trial_health_result_t;
/* After Base's online window, recheck the exact completed event and failure
 * count under the event lock. Freeze events/timers, persist HEALTH_VERIFIED
 * and independently read back. NOT_STARTED permits retry without a write;
 * VERIFIED leaves the guest frozen until OTA VALID and confirm_firmware.
 * UNCERTAIN retains the claim and requires stop/recovery before further work. */
esp_base_container_trial_health_result_t esp_base_container_product_verify_firmware_package_health(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt,
    uint64_t verified_event_sequence, uint64_t verified_failure_count);
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

typedef enum {
    ESP_BASE_CONTAINER_RUN_COMPLETE = 0,
    ESP_BASE_CONTAINER_RUN_REJECTED,
    ESP_BASE_CONTAINER_RUN_BUSY,
    ESP_BASE_CONTAINER_RUN_UNCERTAIN,
} esp_base_container_run_result_t;
/* Boot-local public stop/start: preserve the exact confirmed binding and ECS2
 * sequence. Stop success proves close/join/reclamation; start reopens only a
 * successfully stopped owner, or observes the same live confirmed instance.
 * Trial, trap, blocked and failed-stop owners cannot be restarted here.
 * UNCERTAIN retains the Base claim; no persistence is repaired by this call. */
esp_base_container_run_result_t esp_base_container_product_set_running(
    const esp_base_storage_claim_t *claim, bool running, const char boot_id[37],
    uint32_t expected_sequence, const uint8_t package_sha256[32]);

/* After an EMPTY boot, prove the durable ECS2 record is exactly its initial
 * no-package binding for the currently signed firmware set. A later history,
 * changed firmware identity or unreadable state forbids first ledger creation. */
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

/* Under Base's app/otadata claim, read the durable ECS2 metadata against two
 * observations of the selected signed firmware. During this boot's exact
 * product-only trial, return the still-confirmed old binding and the current
 * ECS2 sequence; the candidate is not reported as confirmed. This is a
 * read-only status/precondition, not package-byte or guest-health proof. The
 * running signed image digest and runtime ABI are independent of the bound
 * package ABI/schema; absent package fields are zero in this C snapshot. */
esp_base_container_binding_result_t esp_base_container_product_binding_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *out);

typedef struct {
    bool present;
    const char *product_id; /* Immutable build authorization, verified on open. */
    size_t product_id_size_bytes; /* Exact authorized ID length; zero when absent. */
    char *product_version; /* Owned copy; caller frees after a successful query. */
    size_t product_version_size_bytes;
    uint8_t package_sha256[32];
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
    bool is_trial;
    char operation_id[37]; /* Empty unless is_trial. */
} esp_base_container_active_product_t;

/* Same firmware observations and ECS2 load as the binding-only precondition.
 * Copy the actual open's full version under the event lock, then match runtime
 * metadata to either the confirmed binding or this exact boot's package trial.
 * Fresh output only: success owns product_version; caller must free it. Failure
 * clears both views and frees any copy. No observed instance yields present=false;
 * that is not a proof of guest health or absence of an unreclaimed native VM. */
esp_base_container_binding_result_t esp_base_container_product_status_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *binding,
    esp_base_container_active_product_t *active);

typedef struct {
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
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

/* Internal preparation step used by the public install/upgrade worker. The
 * caller has durably claimed its operation and holds Base's app/otadata claim.
 * A bounded source supplies exact package bytes; no guest is stopped or
 * started here. Success returns the independently read-back PREPARED sequence.
 * Rejection after reservation returns the read-back ABORTED sequence in the
 * same output; rejection before reservation leaves it zero. BUSY also occurs
 * before reservation and leaves it zero; the caller must verify the old
 * binding and sequence before recording a terminal failure. Uncertainty
 * requires retaining the claim and resolving ECS2 on a new boot. */
esp_base_container_prepare_result_t esp_base_container_product_prepare_package(
    const esp_base_storage_claim_t *claim,
    const esp_base_container_package_request_t *request,
    econtainer_slot_source_fn source_fn, void *source_context,
    uint32_t *prepared_sequence);

/* Cancel only the exact PREPARED package operation before trial begins.
 * The confirmed binding and running old guest are unchanged. The caller
 * retains its claim until ABORTED and both bindings are read back. */
bool esp_base_container_product_abandon_prepared_package(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const uint8_t package_sha256[32], uint32_t *aborted_sequence);

/* Internal same-boot product-only trial. The caller still owns its durable
 * operation and Base claim, and has proven the old instance stopped. Only the
 * exact PREPARED operation may start; the candidate remains unconfirmed until
 * an authorized business event and product-specific health policy succeed. */
esp_base_container_boot_result_t esp_base_container_product_start_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37], const uint8_t trial_event_sha256[32]);

/* Stop and join the candidate, then durably abandon its exact trial and
 * independently read back ABORTED. Success permits reopening the old binding
 * under the same claim. An uncertain result keeps reopening blocked. */
bool esp_base_container_product_abandon_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES]);

typedef enum {
    ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN = 0,
    ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED,
    ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED,
} esp_base_container_package_recovery_t;

/* Before ordinary product_boot on a fresh boot, reconcile one durable
 * PREPARED install/upgrade ledger record. A never-started operation or an
 * exact old-boot candidate is proved failed; the latter is durably abandoned.
 * An exact old-boot CONFIRMED package binding is proved succeeded without
 * replaying the operation. Any uncertain fact leaves the ledger pending. */
esp_base_container_package_recovery_t esp_base_container_product_recover_pending_package(
    const esp_base_storage_claim_t *claim, const char boot_id[37],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t package_sha256[32],
    uint32_t *resolved_sequence);

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
 * The event digest is SHA-256 of the exact copied guest bytes, computed by
 * the authenticated Base ingress. ACCEPTED means queued, not successful. */
esp_base_container_event_result_t esp_base_container_product_offer_event(
    const uint8_t package_sha256[32], uint64_t event_sequence,
    const uint8_t event_sha256[32], const uint8_t *event, size_t size_bytes);
bool esp_base_container_product_event_accepting(void);
/* Counts completed guest calls with runtime OK. The caller must still check
 * the product-specific result; this counter alone is not a trial health proof. */
uint32_t esp_base_container_product_event_progress_count(void);

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

/* Latest completed guest call in this product instance. A negative guest
 * result is a business failure; runtime_ok=false means no guest result was
 * delivered. This volatile observation never confirms product health. */
esp_base_container_event_observation_result_t
esp_base_container_product_event_observation(
    esp_base_container_event_observation_t *out);

/* Accumulated within this exact product-only or firmware/package trial. The representative sequence
 * remains visible after later successful events; any guest/runtime failure
 * clears it and increments failure_count. A busy lock returns false. */
typedef struct {
    uint64_t representative_event_sequence;
    uint64_t failure_count;
    uint8_t package_sha256[32];
} esp_base_container_trial_event_snapshot_t;
bool esp_base_container_product_trial_event_snapshot(
    esp_base_container_trial_event_snapshot_t *out);

/* A read-only hint for the Base policy. False means a queued event or guest
 * call is still active; the confirm operation rechecks under the same lock. */
bool esp_base_container_product_trial_quiescent(void);

/* Internal product-only commit after the Base product policy has independently
 * accepted the exact authorized representative event and its verification
 * window. This checks that the current candidate actually completed that
 * event bytes without a runtime or business failure; it does not define product
 * health. Close event admission only after the queue and current guest call
 * are drained. NOT_STARTED proves no persistent write was attempted; the caller
 * may release its short claim and retry on the next control poll. Once the
 * storage call starts, any unproven result is UNCERTAIN and the long claim must
 * be retained for next-boot recovery. */
typedef enum {
    ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED,
    ESP_BASE_CONTAINER_CONFIRM_CONFIRMED,
    ESP_BASE_CONTAINER_CONFIRM_UNCERTAIN,
} esp_base_container_trial_confirm_result_t;
esp_base_container_trial_confirm_result_t esp_base_container_product_confirm_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint64_t verified_event_sequence, const uint8_t verified_event_sha256[32],
    uint64_t verified_failure_count,
    uint32_t *confirmed_sequence);

typedef enum {
    ESP_BASE_CONTAINER_UNINSTALL_COMPLETE = 0,
    ESP_BASE_CONTAINER_UNINSTALL_NOT_CONFIGURED,
    ESP_BASE_CONTAINER_UNINSTALL_REJECTED,
    ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN,
} esp_base_container_uninstall_result_t;

/* Product operation boundary used by the public uninstall command. The
 * caller first persists its intent, holds the same Base claim used by OTA,
 * and supplies the exact persisted
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

typedef enum {
    ESP_BASE_CONTAINER_UNINSTALL_RECOVERED = 0,
    ESP_BASE_CONTAINER_UNINSTALL_NOT_COMMITTED,
    ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN,
} esp_base_container_uninstall_recovery_t;

/* Fresh-boot read-only resolution of a PREPARED uninstall receipt. The
 * selected signed firmware and complete ECS2 identity set must agree. Exact
 * operation/sequence/empty binding proves completion; the unchanged original
 * sequence/package proves no Container commit. Neither case replays a write. */
esp_base_container_uninstall_recovery_t esp_base_container_product_reconcile_uninstall(
    const esp_base_storage_claim_t *claim,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32]);

/* A complete product policy is required for the persistent binding path. */
bool esp_base_container_product_configured(void);
/* Before any inactive-app write: NO_PACKAGE requires EMPTY, REUSE requires an
 * active confirmed guest, WRITE permits that guest or an admitted EMPTY.
 * Any product/firmware trial or blocked/uninitialized instance is rejected. */
bool esp_base_container_product_ota_ready(esp_base_ota_package_mode_t package_mode);
/* Before normal boot without an active V3 receipt, read the real ECS2 key
 * under Base's claim. A genuinely absent key permits first initialization;
 * any persisted firmware transition requires its original receipt. */
bool esp_base_container_product_without_ota_receipt(
    const esp_base_storage_claim_t *claim);

/* The caller holds Base's app/otadata claim. A CONFIRMED double observation
 * supplies exact signed source/inactive digests. With a product policy, the
 * existing ECS2 blob must reconcile. NO_PACKAGE requires an empty running
 * binding; REUSE/WRITE bind the requested target to the verified running
 * package, enforce schema and slot capacity, and reserve their worst-case
 * ECS2 sequence budget before any app erase.
 * No policy permits NO_PACKAGE only. The public write path currently admits
 * NO_PACKAGE only; the package modes here prepare their read-only snapshot. */
bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim, const esp_base_ota_request_t *request,
    esp_base_ota_receipt_snapshot_t *snapshot);

typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;

/* Called only after eota_retire_inactive has physically erased and read back
 * the exact inactive app under this claim. It accepts the receipt's A/B -> A
 * sequence or an already durable A-only state; the original V3 also binds all
 * source package metadata. A-only confirmed source remains unchanged before
 * stage. PREPARED is never consumed on
 * the OTA worker's same boot. A mismatch retains the claim. */
esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt);

/* Fresh-boot recovery after physical eota_retire_inactive, before product_boot
 * creates any guest thread. The original V3 receipt binds the source package
 * and exact A/C operation. WRITING, PREPARED and old-boot trials are abandoned
 * before the unbootable C binding is dropped. The current boot ID must differ
 * from any recorded trial boot ID; no live guest may be silently canceled. */
esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt,
    const char boot_id[37]);

/* Before starting a guest on selected C, bind the original V3 receipt to the
 * exact A/C ECS2 transition and sequence. A successful prior receipt requires
 * a durable CONFIRMED phase; a PREPARED receipt permits the exact pending
 * trial, or completes HEALTH_VERIFIED -> CONFIRMED for a VALID C and reads it
 * back for all package modes. A historical SUCCEEDED receipt may coexist
 * with a later product-only C operation after full reference reconciliation.
 * No normal boot path changes a transition without this receipt. */
bool esp_base_container_product_reconcile_selected_ota(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt, eota_state_t running_state);

typedef enum {
    ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_STAGE_PREPARED,
    ESP_BASE_CONTAINER_STAGE_WRITING,
    ESP_BASE_CONTAINER_STAGE_REJECTED,
    ESP_BASE_CONTAINER_STAGE_UNCERTAIN,
} esp_base_container_stage_result_t;

/* Called by the OTA worker with its existing claim and its verified original
 * V3 receipt, after eota_prepare and before eota_select. The receipt binds the
 * source, candidate, operation and Container sequence. REUSE verifies the
 * existing signed package for C and commits PREPARED; WRITE only reserves
 * WRITING; a separate verified package write must finish before eota_select. */
esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const esp_base_ota_receipt_recovery_t *receipt);

/* Continue only the exact durable WRITE reservation made by stage_firmware.
 * The caller retains the same OTA owner and supplies bounded package bytes.
 * Before the first erase this rechecks the signed A/C set, original receipt,
 * operation, source binding and WRITING sequence. Success includes package
 * Flash readback, signature/authorization validation and an independent
 * PREPARED readback. Any failure after writing starts is uncertain: retain
 * the owner and resolve the original receipt on a fresh boot. */
esp_base_container_stage_result_t esp_base_container_product_write_staged_firmware_package(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const esp_base_ota_receipt_recovery_t *receipt,
    econtainer_slot_source_fn source_fn, void *source_context);
