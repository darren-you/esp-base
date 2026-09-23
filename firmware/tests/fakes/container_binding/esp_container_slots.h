// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ECONTAINER_SLOT_BINDING_COUNT 2U

typedef enum {
    ECONTAINER_SLOTS_OK = 0,
    ECONTAINER_SLOTS_INVALID,
    ECONTAINER_SLOTS_UNCERTAIN,
    ECONTAINER_SLOTS_BUSY,
    ECONTAINER_SLOTS_UNTRUSTED,
} econtainer_slots_result_t;

typedef struct {
    uint8_t bootable_count;
    uint8_t bootable_firmware_sha256[ECONTAINER_SLOT_BINDING_COUNT][32];
    uint8_t running_firmware_sha256[32];
} econtainer_slot_firmware_set_t;

typedef struct { int unused; } econtainer_slots_io_t;
typedef struct { int unused; } econtainer_slots_geometry_t;
typedef struct { uint32_t sequence; } econtainer_slots_state_t;
typedef enum {
    ECONTAINER_SLOT_BOOT_BLOCKED = 0,
    ECONTAINER_SLOT_BOOT_CONFIRMED,
    ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID,
} econtainer_slot_boot_decision_t;

econtainer_slots_result_t econtainer_slots_reconcile(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision);
