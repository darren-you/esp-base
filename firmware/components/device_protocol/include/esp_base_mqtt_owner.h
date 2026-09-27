// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_base_config.h"
#include "esp_err.h"
#include "emqtt_contract.h"

typedef void (*ebase_mqtt_command_handler_t)(const uint8_t *json, size_t length, void *context);

/* Called only by the Base control task. scratch is distinct from config, is
 * borrowed only until return, and is wiped on every valid call. emqtt_create
 * copies its contents; a new revision replaces the old session. */
esp_err_t esp_base_mqtt_owner_configure(const ebase_mqtt_config_t *config,
                                       const char *device_id, const char *boot_id,
                                       emqtt_config_t *scratch);
/* Revoke the current endpoint and management key before a revision retry. */
esp_err_t esp_base_mqtt_owner_revoke(void);
void esp_base_mqtt_owner_poll(uint64_t now_ms, bool network_ready, bool trusted_time_ready,
                              ebase_mqtt_command_handler_t handler, void *context);
const char *esp_base_mqtt_owner_state(void);
bool esp_base_mqtt_owner_ready(void);
/* Enqueue is transport delivery only; PUBACK cannot make an operation succeed. */
bool esp_base_mqtt_owner_result(const char *json, size_t length);
bool esp_base_mqtt_owner_reported(const char *json, size_t length);
