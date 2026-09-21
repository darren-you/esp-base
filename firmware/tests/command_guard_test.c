// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command_guard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ebase_request_t request(unsigned number)
{
    ebase_request_t r = {.expires_at_ms = 31000};
    snprintf(r.request_id, sizeof r.request_id, "%08x-0000-4000-8000-000000000000", number);
    strcpy(r.device_id, "11111111-1111-4111-8111-111111111111");
    strcpy(r.boot_id, "22222222-2222-4222-8222-222222222222");
    return r;
}

int main(void)
{
    const char short_id[] = "short";
    assert(!ebase_is_uuid(short_id));
    assert(!ebase_is_uuid(NULL));
    ebase_request_guard_t g = {0};
    ebase_request_t r = request(1);
    size_t slot = SIZE_MAX;
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 1000, &slot) == EBASE_ACCEPT && slot == 0);
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, UINT64_MAX, &slot) == EBASE_REPLAY);
    r.fingerprint[0] = 1;
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 1000, &slot) == EBASE_REQUEST_CONFLICT);
    r = request(2);
    assert(ebase_admit(&g, &r, r.device_id, "different-boot", 1000, &slot) == EBASE_WRONG_BOOT);
    assert(ebase_admit(&g, &r, "different-device", r.boot_id, 1000, &slot) == EBASE_WRONG_DEVICE);
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 999, &slot) == EBASE_BAD_WINDOW);
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 31000, &slot) == EBASE_EXPIRED);
    r.expires_at_ms = UINT64_MAX;
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 0, &slot) == EBASE_BAD_WINDOW);
    r = request(2);
    r.request_id[36] = 'a';
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 1000, &slot) == EBASE_BAD_IDENTITY);
    for (unsigned i = 2; i <= EBASE_REQUEST_SLOTS; ++i) {
        r = request(i);
        assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 1000, &slot) == EBASE_ACCEPT);
    }
    r = request(99);
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 1000, &slot) == EBASE_QUEUE_FULL);
    r = request(1);
    assert(ebase_admit(&g, &r, r.device_id, r.boot_id, 1000, &slot) == EBASE_REPLAY);
    assert(g.count == EBASE_REQUEST_SLOTS);
    return 0;
}
