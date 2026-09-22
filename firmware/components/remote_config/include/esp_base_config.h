// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBASE_CONFIG_BYTES 112u

/* P2 station settings. Other capabilities cannot yet be configured.
 * All unused bytes must be zero; never persist a driver struct or C padding. */
typedef struct {
    bool configured;
    char ssid[33];
    char password[65];
} ebase_wifi_config_t;

typedef struct {
    uint32_t revision;
    ebase_wifi_config_t wifi;
} esp_base_remote_config_t;

bool ebase_config_valid(const esp_base_remote_config_t *config);
bool ebase_config_encode(const esp_base_remote_config_t *config, uint8_t out[EBASE_CONFIG_BYTES]);
/* Failed decode leaves out unchanged. NVS supplies integrity checks. */
bool ebase_config_decode(const uint8_t *bytes, size_t length, esp_base_remote_config_t *out);
