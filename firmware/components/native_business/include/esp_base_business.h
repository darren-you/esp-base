// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { EBASE_BUSINESS_IDLE, EBASE_BUSINESS_ACTIVE, EBASE_BUSINESS_PAUSED } ebase_business_state_t;
typedef struct {
    uint32_t byte_count;
    ebase_business_state_t state;
    uint64_t window_deadline_ms;
    uint64_t last_event_sequence;
    int32_t last_result;
    uint8_t last_event_sha256[32];
} ebase_business_t;

/* One control owner executes borrowed authenticated MQTT bytes synchronously.
 * No guest, heap allocation, additional queue, task or retained input exists.
 * Native timers are polled by the same owner; the first count opens 100 ms.
 * Events: 01+nonempty binary body, 02 pause, 03 resume, 04 state, 05 count.
 * The transport enforces its unchanged complete 4096-byte authenticated frame.
 */
void ebase_business_reset(ebase_business_t *business);
void ebase_business_poll(ebase_business_t *business, uint64_t now_ms);
int32_t ebase_business_event(ebase_business_t *business, const uint8_t *bytes,
                             size_t size_bytes, uint64_t now_ms);
