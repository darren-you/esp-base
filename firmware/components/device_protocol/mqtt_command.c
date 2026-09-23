// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_command.h"

#include "esp_base_command_guard.h"
#include <stdio.h>
#include <string.h>

static const char *const channel_names[] = {"command", "result", "reported", "status"};

bool ebase_mqtt_topic(char out[EBASE_MQTT_TOPIC_BYTES], const char *device_id,
                      ebase_mqtt_channel_t channel)
{
    if (!out || !ebase_is_uuid(device_id) || (unsigned)channel >=
            sizeof channel_names / sizeof channel_names[0]) return false;
    const int written = snprintf(out, EBASE_MQTT_TOPIC_BYTES, "esp-base/%s/%s",
                                 device_id, channel_names[channel]);
    return written > 0 && (unsigned)written < EBASE_MQTT_TOPIC_BYTES;
}

static int lower_hex_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool ebase_mqtt_verified_request(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES], const char *device_id,
    const char *topic, uint8_t qos, bool retained,
    const uint8_t *payload, size_t payload_length,
    ebase_mqtt_request_view_t *out)
{
    if (!out) return false;
    *out = (ebase_mqtt_request_view_t){0};
    char expected[EBASE_MQTT_TOPIC_BYTES];
    if (!key || !topic || !payload || qos != 1 || retained ||
        payload_length <= 65 || payload_length > EBASE_MQTT_FRAME_BYTES ||
        !ebase_mqtt_topic(expected, device_id, EBASE_MQTT_COMMAND) ||
        strcmp(topic, expected) || payload[64] != '\n') return false;

    uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES];
    for (size_t i = 0; i < sizeof tag; ++i) {
        const int hi = lower_hex_digit(payload[2 * i]);
        const int lo = lower_hex_digit(payload[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        tag[i] = (uint8_t)((hi << 4) | lo);
    }
    const uint8_t *request = payload + 65;
    const size_t request_length = payload_length - 65;
    if (!ebase_management_authenticate(key, tag, request, request_length)) return false;
    *out = (ebase_mqtt_request_view_t){.request = request, .request_length = request_length};
    return true;
}
