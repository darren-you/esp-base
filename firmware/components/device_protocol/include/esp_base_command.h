// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_command_guard.h"
#include "esp_base_config.h"

#define EBASE_LINE_LIMIT 8192
typedef enum { EBASE_STATUS, EBASE_RESTART, EBASE_CONFIG_SET } ebase_command_kind_t;
typedef struct {
    ebase_command_kind_t kind;
    ebase_request_t request;
    esp_base_remote_config_t config;
} ebase_command_t;

/* The parser never mutates hardware or storage. request_id is empty unless a
 * valid unique UUID was decoded. For CONFIG_SET the transport owner must hash
 * the canonical config bytes before admission. The caller owns all storage. */
const char *ebase_parse_command(const char *json, size_t length, ebase_command_t *out);

typedef struct {
    char data[EBASE_LINE_LIMIT + 1];
    size_t length;
    bool discard;
} ebase_line_reader_t;
typedef void (*ebase_line_handler_t)(const char *line, size_t length, void *context);
/* An invalid/oversized line is drained to LF and delivered as (NULL, 0). */
void ebase_line_feed(ebase_line_reader_t *reader, const void *bytes, size_t length,
                     ebase_line_handler_t handler, void *context);
