// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_ota_stream.h"
#include <limits.h>
#include <string.h>

static bool live(ebase_mqtt_ota_stream_t *s, uint64_t now)
{
    if (!s || !s->active || s->selecting || s->error != EBASE_MQTT_OTA_STREAM_OK) return false;
    if (now < s->last_now || now >= s->total_deadline || now >= s->idle_deadline) {
        s->error = EBASE_MQTT_OTA_STREAM_TIMEOUT;
        return false;
    }
    s->last_now = now;
    return true;
}

bool ebase_mqtt_ota_stream_begin(ebase_mqtt_ota_stream_t *s,
    uint32_t size, uint64_t now, uint32_t total, uint32_t idle)
{
    if (!s || s->active || !size || !total || !idle || idle > total ||
        now > UINT64_MAX - total) return false;
    *s = (ebase_mqtt_ota_stream_t){.size = size, .last_now = now,
        .total_deadline = now + total, .idle_deadline = now + idle,
        .idle_timeout_ms = idle, .active = true};
    return true;
}

bool ebase_mqtt_ota_stream_request(ebase_mqtt_ota_stream_t *s,
    uint64_t now, ebase_mqtt_ota_frame_t *out)
{
    if (!out) return false;
    *out = (ebase_mqtt_ota_frame_t){0};
    if (!live(s, now) || s->ended) return false;
    /* The final partial block needs END before the OTA core flushes its tail.
     * All earlier blocks wait for real Flash acknowledgement. */
    if (s->used != s->length ||
        (s->read_offset < s->size && s->written_offset != s->read_offset)) return false;
    if (!s->request_valid) {
        s->request = (ebase_mqtt_ota_frame_t){.kind = EBASE_MQTT_OTA_REQUEST,
            .offset = s->read_offset, .written_offset = s->written_offset,
            .expires_at_uptime_ms = s->idle_deadline < s->total_deadline ?
                s->idle_deadline : s->total_deadline};
        s->request_valid = true;
    }
    *out = s->request;
    return true;
}

static bool same_request(const ebase_mqtt_ota_frame_t *a, const ebase_mqtt_ota_frame_t *b)
{
    return a->offset == b->offset && a->written_offset == b->written_offset &&
        a->expires_at_uptime_ms == b->expires_at_uptime_ms;
}

ebase_mqtt_ota_admission_t ebase_mqtt_ota_stream_offer(ebase_mqtt_ota_stream_t *s,
    uint64_t now, const ebase_mqtt_ota_frame_t *f)
{
    if (s && s->selecting) return EBASE_MQTT_OTA_IGNORED;
    if (!live(s, now)) return EBASE_MQTT_OTA_REJECTED;
    if (!f || f->kind == EBASE_MQTT_OTA_REQUEST ||
        f->expires_at_uptime_ms <= now) return EBASE_MQTT_OTA_IGNORED;
    if (s->previous_valid && f->kind == s->previous.kind && same_request(f, &s->previous)) {
        if (f->length == s->previous.length && !memcmp(f->tag, s->previous.tag, sizeof f->tag))
            return EBASE_MQTT_OTA_DUPLICATE;
        s->error = EBASE_MQTT_OTA_STREAM_CONFLICT;
        return EBASE_MQTT_OTA_REJECTED;
    }
    if (f->kind == EBASE_MQTT_OTA_CANCEL && !f->length &&
        ((s->request_valid && same_request(f, &s->request)) ||
         (s->previous_valid && same_request(f, &s->previous)))) {
        s->error = EBASE_MQTT_OTA_STREAM_CANCELLED;
        return EBASE_MQTT_OTA_ACCEPTED;
    }
    if (!s->request_valid || !same_request(f, &s->request)) return EBASE_MQTT_OTA_IGNORED;
    if (s->ended || s->used != s->length) return EBASE_MQTT_OTA_IGNORED;
    if (f->kind == EBASE_MQTT_OTA_END) {
        if (s->read_offset != s->size || f->length) return EBASE_MQTT_OTA_IGNORED;
        s->ended = true;
    } else if (f->kind == EBASE_MQTT_OTA_DATA) {
        const uint32_t left = s->size - s->read_offset;
        const size_t expected = left < EBASE_MQTT_OTA_BLOCK_BYTES ? left : EBASE_MQTT_OTA_BLOCK_BYTES;
        if (!expected || !f->data || f->length != expected) return EBASE_MQTT_OTA_IGNORED;
        memcpy(s->block, f->data, f->length);
        s->length = f->length;
        s->used = 0;
    } else return EBASE_MQTT_OTA_IGNORED;
    s->previous = *f;
    s->previous.data = NULL; /* Never retain the MQTT borrowed pointer. */
    s->previous_valid = true;
    s->request_valid = false;
    return EBASE_MQTT_OTA_ACCEPTED;
}

int ebase_mqtt_ota_stream_read(ebase_mqtt_ota_stream_t *s,
    uint64_t now, uint8_t *out, size_t capacity)
{
    if (!out || !capacity || !live(s, now)) return -1;
    if (s->used == s->length) return s->ended && s->read_offset == s->size ? 0 : -2;
    size_t count = s->length - s->used;
    if (count > capacity) count = capacity;
    if (count > INT_MAX) count = INT_MAX;
    memcpy(out, s->block + s->used, count);
    s->used += count;
    s->read_offset += (uint32_t)count;
    return (int)count;
}

bool ebase_mqtt_ota_stream_written(ebase_mqtt_ota_stream_t *s,
    uint64_t now, uint32_t offset)
{
    if (!live(s, now)) return false;
    if (offset < s->written_offset || offset > s->read_offset) {
        s->error = EBASE_MQTT_OTA_STREAM_BAD_WRITE;
        return false;
    }
    if (offset > s->written_offset) {
        s->written_offset = offset;
        /* Saturation at the immutable total deadline avoids clock overflow. */
        const uint64_t left = s->total_deadline - now;
        s->idle_deadline = now + (left < s->idle_timeout_ms ? left : s->idle_timeout_ms);
    }
    return true;
}

bool ebase_mqtt_ota_stream_select(ebase_mqtt_ota_stream_t *s, uint64_t now)
{
    if (!live(s, now) || !s->ended || s->read_offset != s->size ||
        s->written_offset != s->size || s->used != s->length) return false;
    s->selecting = true;
    return true;
}

void ebase_mqtt_ota_stream_close(ebase_mqtt_ota_stream_t *s)
{
    if (!s) return;
    volatile uint8_t *bytes = (volatile uint8_t *)s;
    for (size_t i = 0; i < sizeof *s; ++i) bytes[i] = 0;
}
