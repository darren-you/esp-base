// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_command_guard.h"
#include "esp_base_config.h"
#include "esp_base_ota_policy.h"

#define EBASE_LINE_LIMIT 9216
typedef enum { EBASE_STATUS, EBASE_RESTART, EBASE_CONFIG_SET, EBASE_OTA_START,
    EBASE_OTA_RESULT, EBASE_FIRMWARE_STATUS, EBASE_BUSINESS_STATUS,
    EBASE_BUSINESS_PAUSE, EBASE_BUSINESS_RESUME } ebase_command_kind_t;
typedef struct {
    ebase_command_kind_t kind;
    ebase_request_t request;
    /* Payload storage is allocated only for the selected command kind. The
     * transport copies values or transfers an accepted payload to its owner. */
    size_t payload_size_bytes;
    union {
        void *payload;
        esp_base_remote_config_t *config;
        esp_base_ota_request_t *ota;
        char *operation_id;
    };
} ebase_command_t;

/* The parser never mutates hardware or storage. request_id is empty unless a
 * valid unique UUID was decoded. For CONFIG_SET, OTA_START and business writes,
 * the transport owner must hash canonical values before admission. */
typedef void *(*ebase_command_alloc_t)(size_t size_bytes);
/* Zero-initialize before first use. Parsing releases any previous payload.
 * allocate must return storage that free() can release; the transport chooses
 * its existing memory domain. Release after consuming the borrowed fields. */
const char *ebase_parse_command(const char *json, size_t length,
                                ebase_command_t *out, ebase_command_alloc_t allocate);
void ebase_command_release(ebase_command_t *command);
const char *ebase_parse_frp_command(const char *json, size_t length,
                                   ebase_command_t *out, ebase_command_alloc_t allocate);

/* Read-only FRP status bootstraps current boot/uptime using device/request UUIDs.
 * It leaves boot/deadline zero. The listener authenticates the exact request
 * bytes before parsing and signs the response; write admission is unchanged. */
const char *ebase_parse_frp_status(const char *json, size_t length,
                                  ebase_request_t *out);
/* Same restart identity/fingerprint as USB/MQTT without the large write union. */
const char *ebase_parse_frp_restart(const char *json, size_t length,
                                   ebase_request_t *out);

typedef struct {
    char *data;
    size_t length, capacity;
    bool discard;
} ebase_line_reader_t;
typedef void (*ebase_line_handler_t)(const char *line, size_t length, void *context);
/* Zero-initialize the reader. Storage grows only for actual bytes, up to the
 * same line limit; a completed or rejected line releases it. An invalid,
 * oversized, or allocation-failed line is drained to LF as (NULL, 0).
 * The callback borrows data until return and must not reenter this reader. */
void ebase_line_feed(ebase_line_reader_t *reader, const void *bytes, size_t length,
                     ebase_line_handler_t handler, void *context);

/* Wipes and releases a partial line; safe for an empty reader. */
void ebase_line_release(ebase_line_reader_t *reader);
