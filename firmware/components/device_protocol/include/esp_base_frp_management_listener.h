// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_base_config.h"

typedef enum {
    ESP_BASE_FRP_MANAGEMENT_STATUS,
    ESP_BASE_FRP_MANAGEMENT_RESTART,
} esp_base_frp_management_command_t;

typedef int (*esp_base_frp_management_handler_t)(esp_base_frp_management_command_t command,
                                              const uint8_t *request, size_t length,
                                              char *response, size_t capacity,
                                              size_t *response_length, void *context);

/* Called only by the control owner. Reconfiguration closes the old listener
 * and any partial request before replacing the independent FRP key. */
void esp_base_frp_management_listener_configure(const ebase_frp_config_t *config);
void esp_base_frp_management_listener_poll(uint64_t now_ms,
                                       esp_base_frp_management_handler_t handler,
                                       void *context);
bool esp_base_frp_management_listener_ready(void);
/* Owner-only: a response has been prepared but has not finished sending. */
bool esp_base_frp_management_listener_response_pending(void);
