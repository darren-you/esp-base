// SPDX-License-Identifier: Apache-2.0
#include "esp_base_business.h"
#include <limits.h>
#include <string.h>

void ebase_business_reset(ebase_business_t *business)
{
    if (business) memset(business, 0, sizeof *business);
}

void ebase_business_poll(ebase_business_t *business, uint64_t now_ms)
{
    if (business && business->state == EBASE_BUSINESS_ACTIVE &&
        now_ms >= business->window_deadline_ms) {
        business->state = EBASE_BUSINESS_IDLE;
        business->window_deadline_ms = 0;
    }
}

int32_t ebase_business_event(ebase_business_t *business, const uint8_t *bytes,
                             size_t size_bytes, uint64_t now_ms)
{
    if (!business || !bytes || !size_bytes || size_bytes > 4096U) return -1;
    ebase_business_poll(business, now_ms);
    switch (bytes[0]) {
    case 1:
        if (size_bytes == 1) return -1;
        if (business->state == EBASE_BUSINESS_PAUSED) return -2;
        if (size_bytes - 1U > (uint32_t)INT32_MAX - business->byte_count) return -3;
        if (business->state == EBASE_BUSINESS_IDLE) {
            if (now_ms > UINT64_MAX - 100U) return -4;
            business->window_deadline_ms = now_ms + 100U;
            business->state = EBASE_BUSINESS_ACTIVE;
        }
        business->byte_count += (uint32_t)(size_bytes - 1U);
        return (int32_t)business->byte_count;
    case 2:
        if (size_bytes != 1) return -1;
        business->window_deadline_ms = 0;
        business->state = EBASE_BUSINESS_PAUSED;
        return (int32_t)business->byte_count;
    case 3:
        if (size_bytes != 1 || business->state != EBASE_BUSINESS_PAUSED) return -1;
        business->state = EBASE_BUSINESS_IDLE;
        return (int32_t)business->byte_count;
    case 4: return size_bytes == 1 ? (int32_t)business->state : -1;
    case 5: return size_bytes == 1 ? (int32_t)business->byte_count : -1;
    default: return -1;
    }
}
