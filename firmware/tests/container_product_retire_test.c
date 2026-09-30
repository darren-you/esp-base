// SPDX-License-Identifier: Apache-2.0
/* Host-only product glue test: Container and IDF persistence are fake, while
 * the Base double-observation adapter and owner are the real implementations. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

bool test_policy_enabled = true;
int test_owner_stack_bytes = 16384;
#include "esp_base_container_product.c"

static esp_base_ota_firmware_set_t physical;
static econtainer_slots_state_t persisted;
static unsigned observe_calls;
static unsigned bind_calls;
static unsigned retire_calls;
static unsigned abandon_calls;
static unsigned drop_calls;
static unsigned confirm_calls, confirm_writes;
static bool observation_changes;
static bool load_fails, load_empty, confirm_fails, reconcile_untrusted;
static esp_base_storage_owner_t owner;
static esp_base_storage_owner_t flash_owner;
static esp_base_storage_claim_t claim;
static const char operation_id[] = "11111111-1111-4111-8111-111111111111";
static const char boot_id[] = "22222222-2222-4222-8222-222222222222";

static void fill_sha(uint8_t sha256[32], uint8_t value)
{
    memset(sha256, value, 32);
}

static void physical_set(bool two)
{
    physical = (esp_base_ota_firmware_set_t){0};
    physical.bootable_count = two ? 2U : 1U;
    fill_sha(physical.running_firmware_sha256, 0xa1);
    fill_sha(physical.bootable_firmware_sha256[0], 0xa1);
    if (two) fill_sha(physical.bootable_firmware_sha256[1], 0xb2);
}

static void selected_physical(void)
{
    physical = (esp_base_ota_firmware_set_t){.bootable_count = 2U};
    fill_sha(physical.running_firmware_sha256, 0xc3);
    fill_sha(physical.bootable_firmware_sha256[0], 0xc3);
    fill_sha(physical.bootable_firmware_sha256[1], 0xa1);
}

static void persisted_set(bool two, uint32_t sequence)
{
    persisted = (econtainer_slots_state_t){0};
    persisted.sequence = sequence;
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.bindings[0].present = true;
    fill_sha(persisted.bindings[0].firmware_sha256, 0xa1);
    if (two) {
        persisted.bindings[1].present = true;
        fill_sha(persisted.bindings[1].firmware_sha256, 0xb2);
    }
}

static void fixture(bool configured, bool two)
{
    memset(&s_product, 0, sizeof s_product);
    test_policy_enabled = configured;
    test_owner_stack_bytes = 16384;
    physical_set(two);
    persisted_set(two, 7U);
    observe_calls = bind_calls = retire_calls = abandon_calls = drop_calls = 0U;
    confirm_calls = confirm_writes = 0U;
    observation_changes = load_fails = load_empty = confirm_fails =
        reconcile_untrusted = false;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_owner_init(&flash_owner);
    esp_base_container_product_set_flash_io_owner(&flash_owner);
    claim = (esp_base_storage_claim_t){0};
    assert(esp_base_storage_claim(&owner, &claim));
    if (configured) {
        s_product.provider_bound = true;
        for (size_t index = 0; index < ECONTAINER_SLOT_COUNT; ++index)
            s_product.provider.geometry.slots[index].size_bytes = 0x77000U;
        atomic_store(&s_product.boot_admitted, true);
        atomic_store(&s_product.result, ESP_BASE_CONTAINER_EMPTY);
    }
}

static bool snapshot_ota_mode(const esp_base_storage_claim_t *active_claim,
                              esp_base_ota_package_mode_t mode,
                              esp_base_ota_receipt_snapshot_t *out)
{
    esp_base_ota_request_t request = {.package_mode = mode};
    if (mode != ESP_BASE_OTA_NO_PACKAGE) {
        request.package_size_bytes = 1024U;
        request.guest_abi_version = 2U;
        request.data_schema_version = 1U;
        fill_sha(request.package_sha256, 0xd4);
        fill_sha(request.trial_event_sha256, 0xe5);
    }
    return esp_base_container_product_snapshot_for_ota(active_claim, &request, out);
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation,
    const eota_prepared_t *prepared, esp_base_ota_firmware_set_t *firmware_set)
{
    assert((observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED ||
            observation == ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL) &&
           prepared == NULL);
    *firmware_set = physical;
    ++observe_calls;
    if (observation_changes && (observe_calls % 2U) == 0U)
        firmware_set->running_firmware_sha256[0] ^= 1U;
    return ESP_BASE_OTA_FIRMWARE_OK;
}

static bool fake_set_matches(const econtainer_slot_firmware_set_t *set)
{
    unsigned matched = 0;
    for (unsigned binding = 0; binding < ECONTAINER_SLOT_BINDING_COUNT; ++binding) {
        if (!persisted.bindings[binding].present) continue;
        bool found = false;
        for (unsigned index = 0; index < set->bootable_count; ++index) {
            if (memcmp(persisted.bindings[binding].firmware_sha256,
                       set->bootable_firmware_sha256[index], 32) == 0) {
                if ((matched & (1U << index)) != 0U) return false;
                matched |= 1U << index;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return matched == ((1U << set->bootable_count) - 1U);
}

econtainer_slots_result_t econtainer_slots_load(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    econtainer_slots_state_t *state)
{
    assert(io && geometry && state);
    if (load_fails) return ECONTAINER_SLOTS_IO_FAILED;
    if (load_empty) return ECONTAINER_SLOTS_EMPTY;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_confirm(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t running_firmware_sha256[32],
    const uint8_t current_boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state)
{
    assert(io && geometry && running_firmware_sha256 && current_boot_id && state);
    ++confirm_calls;
    if (confirm_fails) return ECONTAINER_SLOTS_IO_FAILED;
    if (persisted.sequence != expected_sequence ||
        persisted.phase != ECONTAINER_SLOT_HEALTH_VERIFIED ||
        memcmp(persisted.operation.target_firmware_sha256,
               running_firmware_sha256, 32) != 0 ||
        memcmp(persisted.operation.trial_boot_id, current_boot_id,
               ECONTAINER_SLOT_BOOT_ID_BYTES) != 0) return ECONTAINER_SLOTS_CONFLICT;
    persisted.phase = ECONTAINER_SLOT_CONFIRMED;
    ++persisted.sequence;
    ++confirm_writes;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_reconcile(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *set, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision)
{
    assert(io && geometry && set && state && decision);
    *decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    if (load_fails) return ECONTAINER_SLOTS_IO_FAILED;
    if (!fake_set_matches(set)) return ECONTAINER_SLOTS_CONFLICT;
    *state = persisted;
    if (reconcile_untrusted) {
        *decision = ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID;
        return ECONTAINER_SLOTS_UNTRUSTED;
    }
    const bool running_target = memcmp(persisted.operation.target_firmware_sha256,
                                      set->running_firmware_sha256, 32) == 0;
    if (persisted.operation.firmware_transition && running_target &&
        persisted.phase >= ECONTAINER_SLOT_WRITING &&
        persisted.phase <= ECONTAINER_SLOT_HEALTH_VERIFIED) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    *decision = persisted.phase >= ECONTAINER_SLOT_WRITING &&
                persisted.phase <= ECONTAINER_SLOT_HEALTH_VERIFIED ?
                ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED :
                ECONTAINER_SLOT_BOOT_CONFIRMED;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_retire_inactive_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *actual_set,
    const uint8_t retired_sha256[32], econtainer_slots_state_t *state)
{
    assert(io && geometry && state && actual_set && retired_sha256);
    ++retire_calls;
    if (persisted.sequence != expected_sequence ||
        actual_set->bootable_count != 1U ||
        !state_has_firmware(&persisted, actual_set->running_firmware_sha256,
                            retired_sha256, false)) return ECONTAINER_SLOTS_CONFLICT;
    persisted.bindings[1] = (econtainer_slot_binding_t){0};
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.operation = (econtainer_slot_operation_t){0};
    ++persisted.sequence;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_abandon(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t current_boot_id[16],
    econtainer_slot_trial_stopped_fn stopped_fn, void *stopped_context,
    econtainer_slots_state_t *state)
{
    assert(io && geometry && state && current_boot_id);
    assert(stopped_fn == NULL && stopped_context == NULL);
    ++abandon_calls;
    if (persisted.sequence != expected_sequence ||
        persisted.phase < ECONTAINER_SLOT_WRITING ||
        persisted.phase > ECONTAINER_SLOT_HEALTH_VERIFIED)
        return ECONTAINER_SLOTS_CONFLICT;
    persisted.phase = ECONTAINER_SLOT_ABORTED;
    ++persisted.sequence;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_drop_aborted_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *actual_set,
    econtainer_slots_state_t *state)
{
    assert(io && geometry && state && actual_set);
    ++drop_calls;
    if (persisted.sequence != expected_sequence ||
        persisted.phase != ECONTAINER_SLOT_ABORTED ||
        actual_set->bootable_count != 1U) return ECONTAINER_SLOTS_CONFLICT;
    persisted.bindings[1] = (econtainer_slot_binding_t){0};
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.operation = (econtainer_slot_operation_t){0};
    ++persisted.sequence;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

bool econtainer_slots_idf_bind(econtainer_slots_idf_provider_t *provider,
                               const econtainer_slots_idf_config_t *config)
{
    assert(provider && config && config->storage_lock &&
           config->acquire_flash_io && config->release_flash_io &&
           config->flash_io_context == &s_product);
    ++bind_calls;
    memset(provider, 0, sizeof *provider);
    return true;
}

esp_err_t nvs_flash_init_partition(const char *label)
{
    assert(label && label[0]);
    assert(atomic_load(&flash_owner.active_token) != 0U);
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }

static void staged_candidate(uint32_t sequence)
{
    persisted_set(false, sequence);
    persisted.bindings[1].present = true;
    fill_sha(persisted.bindings[1].firmware_sha256, 0xc3);
    persisted.phase = ECONTAINER_SLOT_PREPARED;
    persisted.operation.kind = ECONTAINER_SLOT_NO_PACKAGE;
    persisted.operation.firmware_transition = true;
    fill_sha(persisted.operation.target_firmware_sha256, 0xc3);
    assert(decode_uuid(operation_id, persisted.operation.operation_id));
}

static esp_base_ota_receipt_recovery_t recovery_receipt(
    bool enabled, uint32_t sequence, uint8_t candidate_value,
    const char *operation, bool old_inactive)
{
    esp_base_ota_receipt_recovery_t receipt = {
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .container_enabled = enabled,
        .container_sequence = sequence,
        .package_mode = ESP_BASE_OTA_NO_PACKAGE,
    };
    strcpy(receipt.operation_id, operation);
    fill_sha(receipt.source_sha256, 0xa1);
    if (old_inactive) fill_sha(receipt.inactive_sha256, 0xb2);
    fill_sha(receipt.candidate_sha256, candidate_value);
    return receipt;
}

static esp_base_container_retire_result_t recover(bool enabled, uint32_t sequence,
                                                   uint8_t candidate_value,
                                                   const char *operation)
{
    esp_base_ota_receipt_recovery_t receipt = recovery_receipt(
        enabled, sequence, candidate_value, operation, true);
    return esp_base_container_product_recover_retired_firmware(
        &claim, &receipt, boot_id);
}

static esp_base_ota_receipt_recovery_t selected_receipt(void)
{
    esp_base_ota_receipt_recovery_t receipt = {
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .container_enabled = true,
        .container_sequence = 7U,
    };
    strcpy(receipt.operation_id, operation_id);
    fill_sha(receipt.source_sha256, 0xa1);
    fill_sha(receipt.inactive_sha256, 0xb2);
    fill_sha(receipt.candidate_sha256, 0xc3);
    return receipt;
}

int main(void)
{
    uint8_t source[32], inactive[32];
    fill_sha(source, 0xa1);
    fill_sha(inactive, 0xb2);
    esp_base_ota_receipt_snapshot_t snapshot = {0};

    fixture(false, true);
    assert(!policy_present());
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_WRITE, &snapshot));
    fixture(true, true);
    test_owner_stack_bytes = 8192;
    assert(!configure_policy());
    test_owner_stack_bytes = 16383;
    assert(!configure_policy());
    test_owner_stack_bytes = 16384;
    assert(configure_policy());

    fixture(true, true);
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_enabled && snapshot.container_sequence == 7U &&
           memcmp(snapshot.source_sha256, source, 32) == 0 &&
           memcmp(snapshot.inactive_sha256, inactive, 32) == 0 &&
           observe_calls == 2U);
    persisted.bindings[0].package_present = true;
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_sequence == 0U);

    fixture(true, true);
    atomic_store(&s_product.result, ESP_BASE_CONTAINER_RUNNING);
    atomic_store(&s_product.instance_active, true);
    atomic_store(&s_product.event_accepting, true);
    persisted.bindings[0].package_present = true;
    persisted.bindings[0].package_size_bytes = 1024U;
    persisted.bindings[0].guest_abi_version = 2U;
    persisted.bindings[0].data_schema_version = 1U;
    fill_sha(persisted.bindings[0].package_sha256, 0xd4);
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_REUSE, &snapshot));
    assert(snapshot.container_sequence == 7U && snapshot.source_package_present &&
           snapshot.source_package_size_bytes == 1024U &&
           snapshot.source_guest_abi_version == 2U &&
           snapshot.source_data_schema_version == 1U &&
           snapshot.source_package_sha256[0] == 0xd4);
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_WRITE, &snapshot));
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    esp_base_ota_request_t package_request = {
        .package_mode = ESP_BASE_OTA_PACKAGE_REUSE,
        .package_size_bytes = 1024U,
        .guest_abi_version = 2U,
        .data_schema_version = 1U,
    };
    fill_sha(package_request.package_sha256, 0xd4);
    fill_sha(package_request.trial_event_sha256, 0xe5);
    package_request.package_sha256[0] ^= 1U;
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    assert(snapshot.container_sequence == 0U);
    package_request.package_sha256[0] ^= 1U;
    package_request.data_schema_version = 2U;
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    package_request.data_schema_version = 1U;
    package_request.package_size_bytes = 1025U;
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    package_request.package_mode = ESP_BASE_OTA_PACKAGE_WRITE;
    assert(esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    s_product.provider.geometry.slots[1].size_bytes = 1024U;
    s_product.provider.geometry.slots[2].size_bytes = 1024U;
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    s_product.provider.geometry.slots[1].size_bytes = 0x77000U;
    s_product.provider.geometry.slots[2].size_bytes = 0x77000U;
    package_request.data_schema_version = 2U;
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    package_request.data_schema_version = 1U;
    memset(package_request.trial_event_sha256, 0, 32);
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    package_request = (esp_base_ota_request_t){.package_mode = ESP_BASE_OTA_NO_PACKAGE,
        .package_size_bytes = 1U};
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &package_request, &snapshot));
    atomic_store(&s_product.event_accepting, false);
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_REUSE, &snapshot));
    atomic_store(&s_product.event_accepting, true);
    persisted.bindings[0].package_size_bytes = 0U;
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_REUSE, &snapshot));
    persisted.bindings[0].package_size_bytes = 1024U;
    assert(!snapshot_ota_mode(
        &claim, (esp_base_ota_package_mode_t)-1, &snapshot));
    persisted.sequence = UINT32_MAX - 6U;
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_REUSE, &snapshot));
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_WRITE, &snapshot));
    persisted.sequence = UINT32_MAX - 7U;
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_WRITE, &snapshot));

    fixture(true, false);
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_WRITE, &snapshot));
    assert(!snapshot.source_package_present &&
           snapshot.source_package_size_bytes == 0U);
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_PACKAGE_REUSE, &snapshot));

    /* A/B needs retirement plus five candidate/trial/rollback commits. The
     * first rejected sequence must leave the original bindings untouched. */
    fixture(true, true);
    persisted.sequence = UINT32_MAX - 6U;
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_sequence == UINT32_MAX - 6U &&
           retire_calls == 0U && persisted.bindings[1].present);
    persisted.sequence = UINT32_MAX - 5U;
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_sequence == 0U &&
           persisted.sequence == UINT32_MAX - 5U &&
           retire_calls == 0U && persisted.bindings[1].present);

    /* A-only skips retirement and therefore has exactly one more usable
     * starting sequence. */
    fixture(true, false);
    persisted.sequence = UINT32_MAX - 5U;
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_sequence == UINT32_MAX - 5U &&
           retire_calls == 0U && !persisted.bindings[1].present);
    persisted.sequence = UINT32_MAX - 4U;
    assert(!snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_sequence == 0U &&
           persisted.sequence == UINT32_MAX - 4U &&
           retire_calls == 0U && !persisted.bindings[1].present);

    fixture(false, true);
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(!snapshot.container_enabled && snapshot.container_sequence == 0U &&
           memcmp(snapshot.inactive_sha256, inactive, 32) == 0);
    physical_set(false);
    assert(esp_base_container_product_retire_inactive(&claim, false, 0U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);

    fixture(true, true);
    physical_set(false); /* eota_retire proved first sector erased. */
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 8U && retire_calls == 1U && observe_calls == 2U);
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(retire_calls == 1U);
    assert(esp_base_container_product_retire_inactive(&claim, true, 6U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    observation_changes = true;
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_UNCERTAIN);
    observation_changes = false;

    fixture(true, false);
    uint8_t no_inactive[32] = {0};
    assert(snapshot_ota_mode(
        &claim, ESP_BASE_OTA_NO_PACKAGE, &snapshot));
    assert(snapshot.container_sequence == 7U &&
           memcmp(snapshot.inactive_sha256, no_inactive, 32) == 0);
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, no_inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 7U && retire_calls == 0U);

    fixture(true, true);
    physical_set(false);
    s_product.provider_bound = false;
    atomic_store(&s_product.boot_admitted, false);
    persisted.phase = ECONTAINER_SLOT_CONFIRMED;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 8U && bind_calls == 1U && retire_calls == 1U);
    assert(ensure_provider(&claim) && bind_calls == 1U);

    fixture(true, true);
    physical_set(false);
    s_product.provider_bound = false;
    atomic_store(&s_product.boot_admitted, false);
    staged_candidate(9U); /* S=7, retire=8, stage=9. */
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.phase == ECONTAINER_SLOT_IDLE && persisted.sequence == 11U &&
           abandon_calls == 1U && drop_calls == 1U);
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(abandon_calls == 1U && drop_calls == 1U);

    fixture(true, false);
    s_product.provider_bound = false;
    staged_candidate(8U); /* Original A-only S=7, stage=8. */
    esp_base_ota_receipt_recovery_t no_inactive_receipt = recovery_receipt(
        true, 7U, 0xc3, operation_id, false);
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, &no_inactive_receipt, boot_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 10U && abandon_calls == 1U && drop_calls == 1U);

    /* The admitted boundary leaves exactly enough sequence for rollback
     * after HEALTH_VERIFIED, including both recovery writes. */
    fixture(true, true);
    physical_set(false);
    staged_candidate(UINT32_MAX - 2U); /* S=max-6, retire/stage/trial/health. */
    persisted.phase = ECONTAINER_SLOT_HEALTH_VERIFIED;
    assert(recover(true, UINT32_MAX - 6U, 0xc3, operation_id) ==
           ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == UINT32_MAX &&
           abandon_calls == 1U && drop_calls == 1U);

    fixture(true, false);
    staged_candidate(UINT32_MAX - 2U); /* S=max-5, stage/trial/health. */
    persisted.phase = ECONTAINER_SLOT_HEALTH_VERIFIED;
    no_inactive_receipt = recovery_receipt(
        true, UINT32_MAX - 5U, 0xc3, operation_id, false);
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, &no_inactive_receipt, boot_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == UINT32_MAX &&
           abandon_calls == 1U && drop_calls == 1U);

    fixture(true, true);
    physical_set(false);
    staged_candidate(9U);
    assert(recover(true, 7U, 0xc3,
        "99999999-9999-4999-8999-999999999999") == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    assert(recover(true, 7U, 0xd4, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    ++persisted.sequence;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);

    fixture(true, true);
    physical_set(false);
    staged_candidate(10U);
    persisted.phase = ECONTAINER_SLOT_TRIAL_STARTED;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 12U && abandon_calls == 1U && drop_calls == 1U);

    fixture(true, true);
    physical_set(false);
    s_product.ready = (void *)1;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    assert(retire_calls == 0U);

    fixture(true, true);
    physical_set(false);
    staged_candidate(10U); /* trial began after stage */
    persisted.phase = ECONTAINER_SLOT_TRIAL_STARTED;
    assert(decode_uuid(boot_id, persisted.operation.trial_boot_id));
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);

    fixture(true, true);
    physical_set(false);
    staged_candidate(10U);
    persisted.phase = ECONTAINER_SLOT_ABORTED;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(abandon_calls == 0U && drop_calls == 1U && persisted.sequence == 11U);

    fixture(true, true);
    physical_set(false);
    load_fails = true;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_UNCERTAIN);
    assert(esp_base_storage_claim_active(&claim));

    fixture(true, true);
    selected_physical();
    staged_candidate(9U); /* Original 7, old B retirement 8, C stage 9. */
    esp_base_ota_receipt_recovery_t selected = selected_receipt();
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));
    strcpy(selected.operation_id, "99999999-9999-4999-8999-999999999999");
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));
    selected = selected_receipt();
    ++persisted.sequence;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));

    fixture(true, true);
    selected_physical();
    staged_candidate(11U);
    persisted.phase = ECONTAINER_SLOT_HEALTH_VERIFIED;
    assert(decode_uuid(boot_id, persisted.operation.trial_boot_id));
    selected = selected_receipt();
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    assert(persisted.phase == ECONTAINER_SLOT_CONFIRMED &&
           persisted.sequence == 12U && confirm_writes == 1U);
    selected.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    selected.status = ESP_BASE_OTA_RECEIPT_PREPARED;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    assert(confirm_calls == 1U && confirm_writes == 1U);
    persisted.bindings[0].firmware_sha256[0] ^= 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));

    fixture(true, true);
    selected_physical();
    staged_candidate(12U);
    persisted.phase = ECONTAINER_SLOT_CONFIRMED;
    selected = selected_receipt();
    selected.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    ++persisted.sequence;
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.operation = (econtainer_slot_operation_t){0};
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    persisted.phase = ECONTAINER_SLOT_CONFIRMED;
    persisted.operation.firmware_transition = true;
    memcpy(persisted.operation.target_firmware_sha256,
           selected.candidate_sha256, 32);
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    persisted.operation.firmware_transition = false;
    memcpy(persisted.operation.target_firmware_sha256,
           selected.source_sha256, 32);
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    memcpy(persisted.operation.target_firmware_sha256,
           selected.candidate_sha256, 32);
    reconcile_untrusted = true;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));

    fixture(true, true);
    selected_physical();
    staged_candidate(11U);
    persisted.phase = ECONTAINER_SLOT_HEALTH_VERIFIED;
    selected = selected_receipt();
    selected.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    selected = selected_receipt();
    strcpy(selected.operation_id, "99999999-9999-4999-8999-999999999999");
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    selected = selected_receipt();
    ++selected.container_sequence;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    selected = selected_receipt();
    selected.candidate_sha256[0] ^= 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    assert(confirm_calls == 0U && confirm_writes == 0U &&
           persisted.phase == ECONTAINER_SLOT_HEALTH_VERIFIED);
    selected = selected_receipt();
    confirm_fails = true;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    assert(confirm_calls == 1U && confirm_writes == 0U &&
           persisted.phase == ECONTAINER_SLOT_HEALTH_VERIFIED);

    fixture(true, false);
    assert(esp_base_container_product_without_ota_receipt(&claim));
    staged_candidate(8U);
    assert(!esp_base_container_product_without_ota_receipt(&claim));
    assert(persisted.phase == ECONTAINER_SLOT_PREPARED &&
           confirm_writes == 0U && abandon_calls == 0U && drop_calls == 0U);
    persisted.phase = ECONTAINER_SLOT_CONFIRMED;
    assert(!esp_base_container_product_without_ota_receipt(&claim));
    load_empty = true;
    assert(esp_base_container_product_without_ota_receipt(&claim));
    load_empty = false;
    load_fails = true;
    assert(!esp_base_container_product_without_ota_receipt(&claim));

    fixture(false, true);
    selected_physical();
    selected = selected_receipt();
    selected.container_enabled = false;
    selected.container_sequence = 0U;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_VALID));
    physical.bootable_count = 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));
    selected_physical();
    physical.bootable_firmware_sha256[1][0] ^= 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));
    selected_physical();
    observation_changes = true;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &selected, EOTA_STATE_PENDING_VERIFY));

    puts("  container_product_retire passed (snapshot, retirement, recovery, selected C identity)");
}
