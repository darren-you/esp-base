// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command_guard.h"
#include <string.h>

bool ebase_is_uuid(const char value[EBASE_ID_BYTES])
{
    if (!value) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return value[36] == '\0' && value[14] == '4' && strchr("89ab", value[19]) != NULL;
}

ebase_admission_t ebase_admit(ebase_request_guard_t *g, const ebase_request_t *r,
                             const char *device, const char *boot, uint64_t now,
                             size_t *slot)
{
    if (!g || !r || !device || !boot || !slot || g->count > EBASE_REQUEST_SLOTS ||
        !ebase_is_uuid(r->request_id) || !ebase_is_uuid(r->device_id) || !ebase_is_uuid(r->boot_id))
        return EBASE_BAD_IDENTITY;
    if (strcmp(device, r->device_id)) return EBASE_WRONG_DEVICE;
    if (strcmp(boot, r->boot_id)) return EBASE_WRONG_BOOT;
    for (size_t i = 0; i < g->count; ++i) {
        const ebase_request_t *previous = &g->requests[i];
        if (strcmp(previous->request_id, r->request_id)) continue;
        if (previous->expires_at_ms != r->expires_at_ms ||
            memcmp(previous->fingerprint, r->fingerprint, sizeof r->fingerprint))
            return EBASE_REQUEST_CONFLICT;
        *slot = i;
        return EBASE_REPLAY;
    }
    if (r->expires_at_ms <= now) return EBASE_EXPIRED;
    /* Subtract only after the ordering check, so uint64 deadlines cannot wrap. */
    if (r->expires_at_ms - now > EBASE_REQUEST_WINDOW_MS) return EBASE_BAD_WINDOW;
    if (g->count == EBASE_REQUEST_SLOTS) return EBASE_QUEUE_FULL;
    *slot = g->count;
    g->requests[g->count++] = *r;
    return EBASE_ACCEPT;
}
