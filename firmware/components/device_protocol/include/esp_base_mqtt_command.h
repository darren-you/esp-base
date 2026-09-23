// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_base_network_auth.h"

#define EBASE_MQTT_TOPIC_BYTES 64u
#define EBASE_MQTT_FRAME_BYTES 4096u

typedef enum {
    EBASE_MQTT_COMMAND,
    EBASE_MQTT_RESULT,
    EBASE_MQTT_REPORTED,
    EBASE_MQTT_STATUS
} ebase_mqtt_channel_t;

typedef struct {
    /* Borrowed from the MQTT message. Parse and execute before the next poll. */
    const uint8_t *request;
    size_t request_length;
} ebase_mqtt_request_view_t;

/* All four topics derive only from the durable device UUID. */
bool ebase_mqtt_topic(char out[EBASE_MQTT_TOPIC_BYTES], const char *device_id,
                      ebase_mqtt_channel_t channel);

/* MQTT 3.1.1 command payload: 64 lowercase hex HMAC-SHA256 characters, one LF,
 * then the exact UTF-8 v1 JSON request. The MAC covers only the original JSON
 * bytes. Only a QoS1, non-retained message on this device's exact command
 * topic can yield a view. Authentication precedes JSON decoding and request
 * admission; rejected input must produce no device ACK or request-id echo. */
bool ebase_mqtt_verified_request(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES], const char *device_id,
    const char *topic, uint8_t qos, bool retained,
    const uint8_t *payload, size_t payload_length,
    ebase_mqtt_request_view_t *out);
