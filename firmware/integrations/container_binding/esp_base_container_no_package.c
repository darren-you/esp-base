// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_no_package.h"

#include <string.h>

static bool all_zero(const uint8_t *bytes, size_t length)
{
    uint8_t any = 0U;
    for (size_t index = 0; index < length; ++index) any |= bytes[index];
    return any == 0U;
}

econtainer_slots_result_t esp_base_container_initialize_no_package(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, bool *initialized)
{
    if (initialized != NULL) *initialized = false;
    if (io == NULL || geometry == NULL || firmware_set == NULL || initialized == NULL ||
        firmware_set->bootable_count == 0U ||
        firmware_set->bootable_count > ECONTAINER_SLOT_BINDING_COUNT) {
        return ECONTAINER_SLOTS_INVALID;
    }
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        io, geometry, &state);
    if (loaded != ECONTAINER_SLOTS_EMPTY) return loaded;

    econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT] = {0};
    for (size_t index = 0; index < firmware_set->bootable_count; ++index) {
        bindings[index].present = true;
        memcpy(bindings[index].firmware_sha256,
               firmware_set->bootable_firmware_sha256[index],
               sizeof bindings[index].firmware_sha256);
    }
    const econtainer_slots_result_t result = econtainer_slots_initialize(
        io, geometry, firmware_set, bindings);
    *initialized = result == ECONTAINER_SLOTS_OK;
    return result;
}

econtainer_slots_result_t esp_base_container_pristine_no_package(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set)
{
    if (io == NULL || geometry == NULL || firmware_set == NULL ||
        firmware_set->bootable_count == 0U ||
        firmware_set->bootable_count > ECONTAINER_SLOT_BINDING_COUNT) {
        return ECONTAINER_SLOTS_INVALID;
    }
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(io, geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    const econtainer_slot_operation_t *operation = &state.operation;
    if (state.sequence != 1U || state.phase != ECONTAINER_SLOT_IDLE ||
        !all_zero(operation->operation_id, sizeof operation->operation_id) ||
        !all_zero(operation->target_firmware_sha256, sizeof operation->target_firmware_sha256) ||
        !all_zero(operation->package_sha256, sizeof operation->package_sha256) ||
        !all_zero(operation->trial_boot_id, sizeof operation->trial_boot_id) ||
        operation->kind != ECONTAINER_SLOT_PACKAGE_WRITE || operation->slot != 0U ||
        operation->firmware_transition || operation->package_size_bytes != 0U ||
        operation->guest_abi_version != 0U || operation->data_schema_version != 0U) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    bool running_found = false;
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state.bindings[index];
        if (index >= firmware_set->bootable_count) {
            if (binding->present || binding->package_present || binding->slot != 0U ||
                !all_zero(binding->firmware_sha256, sizeof binding->firmware_sha256) ||
                !all_zero(binding->package_sha256, sizeof binding->package_sha256) ||
                binding->package_size_bytes != 0U || binding->guest_abi_version != 0U ||
                binding->data_schema_version != 0U)
                return ECONTAINER_SLOTS_CONFLICT;
            continue;
        }
        if (!binding->present || binding->package_present || binding->slot != 0U ||
            binding->package_size_bytes != 0U || binding->guest_abi_version != 0U ||
            binding->data_schema_version != 0U ||
            !all_zero(binding->package_sha256, sizeof binding->package_sha256) ||
            memcmp(binding->firmware_sha256,
                   firmware_set->bootable_firmware_sha256[index], 32) != 0) {
            return ECONTAINER_SLOTS_CONFLICT;
        }
        running_found |= memcmp(binding->firmware_sha256,
                                firmware_set->running_firmware_sha256, 32) == 0;
    }
    return running_found ? ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}
