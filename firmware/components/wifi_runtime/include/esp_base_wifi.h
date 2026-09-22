// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_config.h"
#include "esp_err.h"

/* One control task owns every call; SDK callbacks only enqueue bounded events. */
esp_err_t esp_base_wifi_start(const ebase_wifi_config_t *config);
esp_err_t esp_base_wifi_apply(const ebase_wifi_config_t *config, uint64_t now_ms);
void esp_base_wifi_poll(uint64_t now_ms);
const char *esp_base_wifi_state(void);
bool esp_base_wifi_ready(void);
