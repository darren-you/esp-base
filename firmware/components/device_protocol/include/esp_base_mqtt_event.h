// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_base_network_auth.h"

typedef struct {
    uint8_t package_sha256[32];
    uint64_t event_sequence;
    /* Borrowed from the MQTT message until the next poll. */
    const uint8_t *event;
    size_t event_size_bytes;
} ebase_mqtt_event_view_t;

/* QoS 1, non-retained, exact esp-base/<device_id>/event. The wire frame is
 * 64 lowercase hex HMAC-SHA256 bytes, LF, then:
 * "esp-base-product-event-v1\n" || 36 ASCII device UUID || 36 ASCII boot UUID
 * || 32 raw package digest bytes || 8-byte big-endian sequence || guest bytes.
 * The MAC covers the complete bytes after LF. The caller enforces the next
 * sequence and the guest's signed queue/byte limit before acknowledging
 * acceptance; this parser performs no guest call or MQTT publication. */
bool ebase_mqtt_verified_event(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const char *device_id, const char *boot_id, const char *topic,
    uint8_t qos, bool retained, const uint8_t *payload,
    size_t payload_length, ebase_mqtt_event_view_t *out);
