// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_base_config.h"

typedef int (*esp_base_frp_status_handler_t)(const uint8_t *request, size_t length,
                                              char *response, size_t capacity,
                                              size_t *response_length, void *context);

/* Called only by the control owner. Reconfiguration closes the old listener
 * and any partial request before replacing the independent FRP key. */
void esp_base_frp_status_listener_configure(const ebase_frp_config_t *config);
void esp_base_frp_status_listener_poll(uint64_t now_ms,
                                       esp_base_frp_status_handler_t handler,
                                       void *context);
bool esp_base_frp_status_listener_ready(void);
