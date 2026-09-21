// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBASE_REQUEST_SLOTS 32
#define EBASE_ID_BYTES 37
#define EBASE_REQUEST_WINDOW_MS 30000u

typedef enum {
    EBASE_ACCEPT = 0, EBASE_REPLAY, EBASE_BAD_IDENTITY, EBASE_WRONG_DEVICE,
    EBASE_WRONG_BOOT, EBASE_EXPIRED, EBASE_BAD_WINDOW, EBASE_REQUEST_CONFLICT,
    EBASE_QUEUE_FULL
} ebase_admission_t;

typedef struct {
    char request_id[EBASE_ID_BYTES];
    char device_id[EBASE_ID_BYTES];
    char boot_id[EBASE_ID_BYTES];
    uint64_t expires_at_ms;
    uint8_t fingerprint[32];
} ebase_request_t;

typedef struct {
    ebase_request_t requests[EBASE_REQUEST_SLOTS];
    size_t count;
} ebase_request_guard_t;

bool ebase_is_uuid(const char value[EBASE_ID_BYTES]);
/* Called only by the single control owner, immediately before execution.
 * No allocation, no silent eviction. Replays return the existing slot; callers
 * return its saved outcome and MUST NOT perform the action again. */
ebase_admission_t ebase_admit(ebase_request_guard_t *guard, const ebase_request_t *request,
                             const char *device_id, const char *boot_id, uint64_t now_ms,
                             size_t *slot);
