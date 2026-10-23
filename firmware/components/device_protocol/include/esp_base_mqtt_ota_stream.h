// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_mqtt_ota.h"

typedef enum {
    EBASE_MQTT_OTA_STREAM_OK,
    EBASE_MQTT_OTA_STREAM_TIMEOUT,
    EBASE_MQTT_OTA_STREAM_CANCELLED,
    EBASE_MQTT_OTA_STREAM_CONFLICT,
    EBASE_MQTT_OTA_STREAM_BAD_WRITE
} ebase_mqtt_ota_stream_error_t;

typedef enum {
    EBASE_MQTT_OTA_IGNORED,
    EBASE_MQTT_OTA_ACCEPTED,
    EBASE_MQTT_OTA_DUPLICATE,
    EBASE_MQTT_OTA_REJECTED
} ebase_mqtt_ota_admission_t;

/* One fixed block, no heap, NVS, MQTT calls or Flash I/O. The caller serializes
 * these short methods and captures now_ms inside that critical section. MQTT
 * authentication/identity binding precedes offer(); the owning operation must
 * not be replaced while a worker can still read or acknowledge this state. */
typedef struct {
    uint8_t block[EBASE_MQTT_OTA_BLOCK_BYTES];
    size_t used, length;
    uint32_t size, read_offset, written_offset;
    uint64_t last_now, total_deadline, idle_deadline;
    uint32_t idle_timeout_ms;
    ebase_mqtt_ota_frame_t request;
    ebase_mqtt_ota_frame_t previous;
    bool active, request_valid, previous_valid, ended, selecting;
    ebase_mqtt_ota_stream_error_t error;
} ebase_mqtt_ota_stream_t;

bool ebase_mqtt_ota_stream_begin(ebase_mqtt_ota_stream_t *stream,
    uint32_t image_size, uint64_t now_ms, uint32_t total_timeout_ms,
    uint32_t idle_timeout_ms);
/* Returns the same immutable request until that block is written. The caller
 * rate-limits publication. Reconnect/republication never refreshes deadlines. */
bool ebase_mqtt_ota_stream_request(ebase_mqtt_ota_stream_t *stream,
    uint64_t now_ms, ebase_mqtt_ota_frame_t *out);
ebase_mqtt_ota_admission_t ebase_mqtt_ota_stream_offer(ebase_mqtt_ota_stream_t *stream,
    uint64_t now_ms, const ebase_mqtt_ota_frame_t *verified);
/* Nonblocking: positive copied bytes, 0 only after authenticated END,
 * -2 while waiting and -1 on a latched failure. */
int ebase_mqtt_ota_stream_read(ebase_mqtt_ota_stream_t *stream,
    uint64_t now_ms, uint8_t *out, size_t capacity);
/* Called after successful sequential Flash writes and short-claim release,
 * never from the legacy receive-progress callback or a Broker PUBACK. */
bool ebase_mqtt_ota_stream_written(ebase_mqtt_ota_stream_t *stream,
    uint64_t now_ms, uint32_t written_offset);
/* Atomic cancellation boundary after prepare verifies digest/signature. Only
 * a complete, live stream may begin boot selection. Later network frames have
 * no effect; cancellation must query the original durable operation result. */
bool ebase_mqtt_ota_stream_select(ebase_mqtt_ota_stream_t *stream, uint64_t now_ms);
/* Only after the worker has stopped; does not modify the durable V4 receipt. */
void ebase_mqtt_ota_stream_close(ebase_mqtt_ota_stream_t *stream);
