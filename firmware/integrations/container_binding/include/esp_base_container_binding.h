// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_storage_owner.h"
#include "esp_container_slots.h"

/* The callback may perform one Container operation while the same Base owner
 * used by OTA remains claimed. It must not retain the firmware-set pointer or
 * write an app partition/otadata. The Container provider's own storage lock
 * remains distinct and is acquired inside its operation. */
typedef econtainer_slots_result_t (*esp_base_container_operation_fn)(
    const econtainer_slot_firmware_set_t *firmware_set, void *context);

econtainer_slots_result_t esp_base_container_with_firmware_set(
    esp_base_storage_owner_t *owner, esp_base_container_operation_fn operation,
    void *context);

/* Startup selection is blocked until the physical signed firmware set and
 * every persisted Container binding agree. This does not install packages. */
econtainer_slots_result_t esp_base_container_reconcile(
    esp_base_storage_owner_t *owner, const econtainer_slots_io_t *io,
    const econtainer_slots_geometry_t *geometry, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision);
