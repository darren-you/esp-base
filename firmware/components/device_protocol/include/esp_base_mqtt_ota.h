// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_base_network_auth.h"

#define EBASE_MQTT_OTA_BLOCK_BYTES 1024u
#define EBASE_MQTT_OTA_DOMAIN "esp-base-ota-v1\n"
#define EBASE_MQTT_OTA_HEADER_BYTES (32u + sizeof(EBASE_MQTT_OTA_DOMAIN) - 1u + 1u + 108u + 8u + 8u + 2u)
#define EBASE_MQTT_OTA_FRAME_BYTES (EBASE_MQTT_OTA_HEADER_BYTES + EBASE_MQTT_OTA_BLOCK_BYTES)

typedef enum {
    EBASE_MQTT_OTA_REQUEST = 1,
    EBASE_MQTT_OTA_DATA = 2,
    EBASE_MQTT_OTA_END = 3,
    EBASE_MQTT_OTA_CANCEL = 4
} ebase_mqtt_ota_kind_t;

typedef struct {
    ebase_mqtt_ota_kind_t kind;
    uint32_t offset;
    uint32_t written_offset;
    uint64_t expires_at_uptime_ms;
    /* Borrowed bytes expire with the original MQTT message. */
    const uint8_t *data;
    size_t length;
    /* Exact authenticated frame identity, for duplicate/conflict admission. */
    uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES];
} ebase_mqtt_ota_frame_t;

/* Binary HMAC-SHA256 || domain || kind || device/boot/operation UUIDs (36
 * ASCII bytes each) || BE32 offset || BE32 written offset || BE64 expiry
 * || BE16 length || data. Offset requests bytes; written offset acknowledges
 * completed Flash writes. A final partial block can request END before its
 * write is acknowledged. Neither value is a durable OTA terminal result.
 * Only REQUEST uses ota/request; DATA/END/CANCEL use ota/data. All are QoS1,
 * non-retained. Decoder clears out on every failure. It authenticates exact
 * bytes and binds identities/direction, but does not execute or advance OTA.
 * The active transfer separately checks requested offset, expiry and length. */
bool ebase_mqtt_ota_decode(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const char *device_id, const char *boot_id, const char *operation_id,
    bool from_device, const char *topic, uint8_t qos, bool retained,
    const uint8_t *payload, size_t length, ebase_mqtt_ota_frame_t *out);

bool ebase_mqtt_ota_encode(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const char *device_id, const char *boot_id, const char *operation_id,
    const ebase_mqtt_ota_frame_t *frame, uint8_t *out, size_t capacity,
    size_t *length);
