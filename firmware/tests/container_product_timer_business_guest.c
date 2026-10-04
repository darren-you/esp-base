// SPDX-License-Identifier: Apache-2.0
#include "econtainer_guest.h"

#ifndef ECONTAINER_TIMER_BUSINESS_RESULT
#define ECONTAINER_TIMER_BUSINESS_RESULT (-7)
#endif

static uint32_t timer_calls;

int32_t econtainer_init(void)
{
    timer_calls = 0U;
    return 0;
}

int32_t econtainer_on_event(const uint8_t *bytes, uint32_t size_bytes)
{
    uint64_t handle = 0;
    uint32_t skipped = 0;
    if (econtainer_timer_event_decode(bytes, size_bytes, &handle, &skipped)) {
        ++timer_calls;
        return ECONTAINER_TIMER_BUSINESS_RESULT;
    }
    if (bytes == 0 || size_bytes != 1U) return -1;
    if (bytes[0] == 'R') return 7;
    if (bytes[0] == 'T') return econtainer_timer_start(10U, 0U) != 0U ? 0 : -2;
    if (bytes[0] == 'Q') return (int32_t)timer_calls;
    return -1;
}

int32_t econtainer_stop(void)
{
    return 0;
}
