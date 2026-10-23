// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_ota.h"
#include "esp_base_command_guard.h"
#include "esp_base_mqtt_command.h"
#include <stdio.h>
#include <string.h>

_Static_assert(EBASE_MQTT_OTA_FRAME_BYTES <= EBASE_MQTT_FRAME_BYTES,
               "OTA authentication frame exceeds MQTT admission");

static bool valid_frame(const ebase_mqtt_ota_frame_t *frame)
{
    if (!frame || !frame->expires_at_uptime_ms || frame->written_offset > frame->offset ||
        frame->length > EBASE_MQTT_OTA_BLOCK_BYTES) return false;
    switch (frame->kind) {
    case EBASE_MQTT_OTA_DATA: return frame->data && frame->length > 0;
    case EBASE_MQTT_OTA_REQUEST:
    case EBASE_MQTT_OTA_END:
    case EBASE_MQTT_OTA_CANCEL: return frame->length == 0;
    default: return false;
    }
}

static void put_be(uint8_t *out, uint64_t value, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        out[i] = (uint8_t)(value >> ((length - i - 1u) * 8u));
}

static uint64_t get_be(const uint8_t *bytes, size_t length)
{
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) value = (value << 8u) | bytes[i];
    return value;
}

bool ebase_mqtt_ota_encode(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const char *device_id, const char *boot_id, const char *operation_id,
    const ebase_mqtt_ota_frame_t *frame, uint8_t *out, size_t capacity,
    size_t *length)
{
    if (!length) return false;
    *length = 0;
    if (!key || !out || !ebase_is_uuid(device_id) || !ebase_is_uuid(boot_id) ||
        !ebase_is_uuid(operation_id) || !valid_frame(frame) ||
        capacity < EBASE_MQTT_OTA_HEADER_BYTES + frame->length) return false;
    uint8_t *cursor = out + EBASE_MANAGEMENT_TAG_BYTES;
    memcpy(cursor, EBASE_MQTT_OTA_DOMAIN, sizeof(EBASE_MQTT_OTA_DOMAIN) - 1u);
    cursor += sizeof(EBASE_MQTT_OTA_DOMAIN) - 1u;
    *cursor++ = (uint8_t)frame->kind;
    memcpy(cursor, device_id, 36); cursor += 36;
    memcpy(cursor, boot_id, 36); cursor += 36;
    memcpy(cursor, operation_id, 36); cursor += 36;
    put_be(cursor, frame->offset, 4); cursor += 4;
    put_be(cursor, frame->written_offset, 4); cursor += 4;
    put_be(cursor, frame->expires_at_uptime_ms, 8); cursor += 8;
    put_be(cursor, frame->length, 2); cursor += 2;
    if (frame->length) memcpy(cursor, frame->data, frame->length);
    const size_t size = EBASE_MQTT_OTA_HEADER_BYTES + frame->length;
    if (!ebase_management_sign(key, out + EBASE_MANAGEMENT_TAG_BYTES,
                               size - EBASE_MANAGEMENT_TAG_BYTES, out)) return false;
    *length = size;
    return true;
}

bool ebase_mqtt_ota_decode(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const char *device_id, const char *boot_id, const char *operation_id,
    bool from_device, const char *topic, uint8_t qos, bool retained,
    const uint8_t *payload, size_t length, ebase_mqtt_ota_frame_t *out)
{
    if (!out) return false;
    *out = (ebase_mqtt_ota_frame_t){0};
    if (!key || !topic || !payload || qos != 1 || retained ||
        !ebase_is_uuid(device_id) || !ebase_is_uuid(boot_id) || !ebase_is_uuid(operation_id) ||
        length < EBASE_MQTT_OTA_HEADER_BYTES || length > EBASE_MQTT_OTA_FRAME_BYTES) return false;
    char expected[EBASE_MQTT_TOPIC_BYTES];
    const int n = snprintf(expected, sizeof expected, "esp-base/%s/ota/%s",
        device_id, from_device ? "request" : "data");
    if (n <= 0 || (size_t)n >= sizeof expected || strcmp(topic, expected) ||
        !ebase_management_authenticate(key, payload, payload + EBASE_MANAGEMENT_TAG_BYTES,
                                       length - EBASE_MANAGEMENT_TAG_BYTES)) return false;
    const uint8_t *cursor = payload + EBASE_MANAGEMENT_TAG_BYTES;
    if (memcmp(cursor, EBASE_MQTT_OTA_DOMAIN, sizeof(EBASE_MQTT_OTA_DOMAIN) - 1u)) return false;
    cursor += sizeof(EBASE_MQTT_OTA_DOMAIN) - 1u;
    ebase_mqtt_ota_frame_t frame = {.kind = (ebase_mqtt_ota_kind_t)*cursor++};
    if ((frame.kind == EBASE_MQTT_OTA_REQUEST) != from_device ||
        memcmp(cursor, device_id, 36) || memcmp(cursor + 36, boot_id, 36) ||
        memcmp(cursor + 72, operation_id, 36)) return false;
    cursor += 108;
    frame.offset = (uint32_t)get_be(cursor, 4); cursor += 4;
    frame.written_offset = (uint32_t)get_be(cursor, 4); cursor += 4;
    frame.expires_at_uptime_ms = get_be(cursor, 8); cursor += 8;
    frame.length = (size_t)get_be(cursor, 2); cursor += 2;
    frame.data = frame.length ? cursor : NULL;
    if (frame.length != length - EBASE_MQTT_OTA_HEADER_BYTES || !valid_frame(&frame)) return false;
    memcpy(frame.tag, payload, sizeof frame.tag);
    *out = frame;
    return true;
}
