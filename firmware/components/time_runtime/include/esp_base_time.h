// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Boot-lifetime SNTP owner. The server is copied before the SDK sees it.
 * Start only after esp_netif and the default event loop are initialized. */
esp_err_t esp_base_time_start(const char *server);

/* Nonblocking; call from the single control task. A previous successful sync
 * stays usable through Wi-Fi reconnect while the device clock keeps running. */
void esp_base_time_poll(void);

/* False until this boot has received a valid SNTP sync. */
bool esp_base_time_ready(void);
