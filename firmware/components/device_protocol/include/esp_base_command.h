// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_command_guard.h"
#include "esp_base_config.h"
#include "esp_base_ota_policy.h"
#include "esp_base_product_package_source.h"

#define EBASE_LINE_LIMIT 9216
typedef enum { EBASE_STATUS, EBASE_RESTART, EBASE_CONFIG_SET, EBASE_OTA_START, EBASE_OTA_RESULT, EBASE_PRODUCT_STATUS, EBASE_PRODUCT_RESULT, EBASE_PRODUCT_UNINSTALL_COMMAND, EBASE_PRODUCT_INSTALL_COMMAND, EBASE_PRODUCT_UPGRADE_COMMAND } ebase_command_kind_t;
typedef struct {
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint32_t operation_sequence;
    uint32_t expected_container_sequence;
    uint8_t package_sha256[32];
} ebase_product_uninstall_request_t;
typedef struct {
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint32_t operation_sequence;
    uint32_t expected_container_sequence;
    bool previous_package_present;
    uint8_t previous_package_sha256[32];
    uint8_t package_sha256[32];
    uint8_t trial_event_sha256[32];
    uint32_t package_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
    char package_url[ESP_BASE_PRODUCT_PACKAGE_URL_BYTES + 1U];
} ebase_product_package_request_t;
typedef struct {
    ebase_command_kind_t kind;
    ebase_request_t request;
    /* Each command kind owns exactly one payload. The control task copies
     * config/OTA data into their independent long-lived storage before another
     * command is parsed; status and restart have no payload. */
    union {
        esp_base_remote_config_t config;
        esp_base_ota_request_t ota;
        char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
        ebase_product_uninstall_request_t product_uninstall;
        ebase_product_package_request_t product_package;
    };
} ebase_command_t;

/* The parser never mutates hardware or storage. request_id is empty unless a
 * valid unique UUID was decoded. For CONFIG_SET, OTA_START and product writes,
 * the transport owner must hash canonical values before admission. */
const char *ebase_parse_command(const char *json, size_t length, ebase_command_t *out);

/* Authenticated FRP status uses the write-command identity and uptime window
 * even though it cannot mutate device state. The listener authenticates the
 * exact bytes before calling this parser. */
const char *ebase_parse_frp_status(const char *json, size_t length,
                                  ebase_request_t *out);

typedef struct {
    char data[EBASE_LINE_LIMIT + 1];
    size_t length;
    bool discard;
} ebase_line_reader_t;
typedef void (*ebase_line_handler_t)(const char *line, size_t length, void *context);
/* An invalid/oversized line is drained to LF and delivered as (NULL, 0). */
void ebase_line_feed(ebase_line_reader_t *reader, const void *bytes, size_t length,
                     ebase_line_handler_t handler, void *context);
