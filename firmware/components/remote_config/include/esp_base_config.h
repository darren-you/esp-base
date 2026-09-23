// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBASE_CONFIG_HEADER_BYTES 24u
#define EBASE_CONFIG_MAX_BYTES 4885u
#define EBASE_MQTT_KEY_BYTES 32u

/* Persistent schema v2 has one canonical, variable-length EBCF blob. No
 * runtime v1 reader or implicit credential generation is permitted. */
typedef struct {
    bool configured;
    char ssid[33];
    char password[65];
} ebase_wifi_config_t;

typedef struct {
    bool configured;
    char hostname[254];
    uint16_t port;
    char username[129];
    char password[257];
    char ca_pem[4097];
    uint8_t management_key[EBASE_MQTT_KEY_BYTES];
} ebase_mqtt_config_t;

typedef struct {
    uint32_t revision;
    ebase_wifi_config_t wifi;
    ebase_mqtt_config_t mqtt;
} esp_base_remote_config_t;

bool ebase_wifi_config_valid(const ebase_wifi_config_t *wifi);
bool ebase_config_valid(const esp_base_remote_config_t *config);
/* On success, written is the exact canonical length. The output buffer must
 * hold EBASE_CONFIG_MAX_BYTES, but only written bytes are persisted/hashed. */
bool ebase_config_encode(const esp_base_remote_config_t *config,
                         uint8_t out[EBASE_CONFIG_MAX_BYTES], size_t *written);
/* Failed decode leaves out unchanged. NVS supplies integrity checks. */
bool ebase_config_decode(const uint8_t *bytes, size_t length, esp_base_remote_config_t *out);
