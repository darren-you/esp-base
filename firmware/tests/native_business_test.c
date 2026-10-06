// SPDX-License-Identifier: Apache-2.0
#include "esp_base_business.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    ebase_business_t b;
    ebase_business_reset(&b);
    const uint8_t message[] = {1, 0, 0xff}, count[] = {5}, state[] = {4}, pause[] = {2}, resume[] = {3};
    assert(ebase_business_event(&b, message, sizeof message, 10) == 2);
    assert(ebase_business_event(&b, count, 1, 11) == 2);
    assert(ebase_business_event(&b, message, sizeof message, 12) == 4);
    assert(b.window_deadline_ms == 110);
    assert(ebase_business_event(&b, pause, 1, 13) == 4);
    assert(ebase_business_event(&b, state, 1, 14) == 2);
    assert(ebase_business_event(&b, message, sizeof message, 15) == -2);
    assert(ebase_business_event(&b, count, 1, 16) == 4);
    assert(ebase_business_event(&b, resume, 1, 17) == 4);
    assert(ebase_business_event(&b, state, 1, 18) == 0);
    assert(ebase_business_event(&b, message, sizeof message, 20) == 6);
    assert(ebase_business_event(&b, state, 1, 119) == 1);
    assert(ebase_business_event(&b, state, 1, 120) == 0);
    assert(ebase_business_event(&b, count, 1, 121) == 6);
    uint8_t maximum[4096]; memset(maximum, 0, sizeof maximum); maximum[0] = 1;
    assert(ebase_business_event(&b, maximum, sizeof maximum, 130) == 4101);
    assert(ebase_business_event(&b, maximum, sizeof maximum + 1U, 130) == -1);
    const uint8_t invalid[] = {9}; assert(ebase_business_event(&b, invalid, 1, 130) == -1);
    assert(b.byte_count == 4101);
    b.byte_count = INT32_MAX; assert(ebase_business_event(&b, message, 3, 131) == -3);
    assert(b.byte_count == INT32_MAX);
    ebase_business_reset(&b); assert(b.byte_count == 0 && b.state == EBASE_BUSINESS_IDLE);
    assert(ebase_business_event(&b, message, 3, UINT64_MAX) == -4);
    assert(b.byte_count == 0 && b.state == EBASE_BUSINESS_IDLE);
    puts("native_business: twelve business behaviors, binary maximum, timer and failure invariants passed");
}
