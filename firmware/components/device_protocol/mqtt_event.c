// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_event.h"

#include <string.h>

#include "esp_base_command_guard.h"
#include "esp_base_mqtt_command.h"

static const uint8_t domain[] = "esp-base-product-event-v1\n";

static int lower_hex_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool ebase_mqtt_verified_event(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const char *device_id, const char *boot_id, const char *topic,
    uint8_t qos, bool retained, const uint8_t *payload,
    size_t payload_length, ebase_mqtt_event_view_t *out)
{
    if (out == NULL) return false;
    *out = (ebase_mqtt_event_view_t){0};
    char expected_topic[EBASE_MQTT_TOPIC_BYTES];
    enum { TAG_HEX_BYTES = 64, TAG_FRAME_BYTES = 65, UUID_BYTES = 36 };
    const size_t header_bytes = sizeof domain - 1U + UUID_BYTES * 2U + 32U + 8U;
    if (key == NULL || topic == NULL || payload == NULL ||
        !ebase_is_uuid(device_id) || !ebase_is_uuid(boot_id) ||
        qos != 1U || retained || payload_length <= TAG_FRAME_BYTES + header_bytes ||
        payload_length > EBASE_MQTT_FRAME_BYTES || payload[TAG_HEX_BYTES] != '\n' ||
        !ebase_mqtt_topic(expected_topic, device_id, EBASE_MQTT_EVENT) ||
        strcmp(topic, expected_topic) != 0) return false;

    uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES];
    for (size_t index = 0; index < sizeof tag; ++index) {
        const int high = lower_hex_digit(payload[index * 2U]);
        const int low = lower_hex_digit(payload[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        tag[index] = (uint8_t)((high << 4) | low);
    }
    const uint8_t *signed_bytes = payload + TAG_FRAME_BYTES;
    const size_t signed_size_bytes = payload_length - TAG_FRAME_BYTES;
    if (!ebase_management_authenticate(key, tag, signed_bytes,
                                       signed_size_bytes)) return false;
    if (memcmp(signed_bytes, domain, sizeof domain - 1U) != 0) return false;
    const uint8_t *cursor = signed_bytes + sizeof domain - 1U;
    if (memcmp(cursor, device_id, UUID_BYTES) != 0) return false;
    cursor += UUID_BYTES;
    if (memcmp(cursor, boot_id, UUID_BYTES) != 0) return false;
    cursor += UUID_BYTES;
    uint8_t digest_or = 0;
    for (size_t index = 0; index < 32U; ++index) digest_or |= cursor[index];
    if (digest_or == 0U) return false;
    memcpy(out->package_sha256, cursor, 32U);
    cursor += 32U;
    uint64_t sequence = 0;
    for (size_t index = 0; index < 8U; ++index) {
        sequence = (sequence << 8U) | cursor[index];
    }
    if (sequence == 0U) {
        *out = (ebase_mqtt_event_view_t){0};
        return false;
    }
    cursor += 8U;
    out->event_sequence = sequence;
    out->event = cursor;
    out->event_size_bytes = signed_size_bytes - header_bytes;
    return true;
}
