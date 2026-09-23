// SPDX-License-Identifier: Apache-2.0
#include "esp_base_time.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "esp_netif_sntp.h"

#define ESP_BASE_TIME_SERVER_MAX_BYTES 253
#define ESP_BASE_TIME_MIN_UNIX_SECONDS ((time_t)1704067200) /* 2024-01-01 UTC */

static char s_server[ESP_BASE_TIME_SERVER_MAX_BYTES + 1];
static atomic_bool s_started;
static atomic_bool s_synchronized;

esp_err_t esp_base_time_start(const char *server)
{
    if (atomic_load_explicit(&s_started, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = 0;
    while (length <= ESP_BASE_TIME_SERVER_MAX_BYTES && server[length] != '\0') {
        ++length;
    }
    if (length == 0 || length > ESP_BASE_TIME_SERVER_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_server, server, length + 1);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server);
    const esp_err_t result = esp_netif_sntp_init(&config);
    if (result != ESP_OK) {
        memset(s_server, 0, sizeof s_server);
        return result;
    }
    atomic_store_explicit(&s_synchronized, false, memory_order_release);
    atomic_store_explicit(&s_started, true, memory_order_release);
    return ESP_OK;
}

void esp_base_time_poll(void)
{
    if (!atomic_load_explicit(&s_started, memory_order_acquire)) {
        return;
    }
    const esp_err_t result = esp_netif_sntp_sync_wait(0);
    if (result == ESP_OK) {
        atomic_store_explicit(&s_synchronized, time(NULL) >= ESP_BASE_TIME_MIN_UNIX_SECONDS,
                              memory_order_release);
    } else if (result == ESP_ERR_INVALID_STATE) {
        atomic_store_explicit(&s_synchronized, false, memory_order_release);
    }
}

bool esp_base_time_ready(void)
{
    return atomic_load_explicit(&s_synchronized, memory_order_acquire) &&
           time(NULL) >= ESP_BASE_TIME_MIN_UNIX_SECONDS;
}
