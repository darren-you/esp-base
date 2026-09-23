#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool smooth_sync;
    bool server_from_dhcp;
    bool wait_for_sync;
    bool start;
    const char *servers[1];
    size_t num_of_servers;
} esp_sntp_config_t;

#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) \
    { .smooth_sync = false, .server_from_dhcp = false, .wait_for_sync = true, \
      .start = true, .servers = {server}, .num_of_servers = 1 }

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout);
