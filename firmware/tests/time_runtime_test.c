// SPDX-License-Identifier: Apache-2.0
#include "esp_base_time.h"
#include "esp_netif_sntp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static esp_err_t init_result = ESP_FAIL;
static esp_err_t wait_result = ESP_ERR_TIMEOUT;
static unsigned init_calls, wait_calls;
static const char *saved_server;
static time_t wall_seconds = 1704067200;

time_t time(time_t *result)
{
    if (result != NULL) {
        *result = wall_seconds;
    }
    return wall_seconds;
}

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    ++init_calls;
    assert(config->wait_for_sync && config->start && !config->smooth_sync);
    assert(!config->server_from_dhcp && config->num_of_servers == 1);
    saved_server = config->servers[0];
    return init_result;
}

esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout)
{
    assert(timeout == 0);
    ++wait_calls;
    return wait_result;
}

int main(void)
{
    esp_base_time_poll();
    assert(wait_calls == 0 && !esp_base_time_ready());
    assert(esp_base_time_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(esp_base_time_start("") == ESP_ERR_INVALID_ARG);
    char too_long[255];
    memset(too_long, 'x', sizeof too_long);
    too_long[sizeof too_long - 1] = '\0';
    assert(esp_base_time_start(too_long) == ESP_ERR_INVALID_ARG);
    assert(init_calls == 0);

    assert(esp_base_time_start("time.example.invalid") == ESP_FAIL);
    assert(init_calls == 1 && !esp_base_time_ready());

    init_result = ESP_OK;
    char server[] = "time.example.invalid";
    assert(esp_base_time_start(server) == ESP_OK);
    memset(server, 'x', sizeof server - 1);
    assert(!strcmp(saved_server, "time.example.invalid"));
    assert(esp_base_time_start("another.invalid") == ESP_ERR_INVALID_STATE);
    assert(init_calls == 2 && !esp_base_time_ready());

    esp_base_time_poll();
    assert(wait_calls == 1 && !esp_base_time_ready());
    wait_result = ESP_OK;
    wall_seconds = 1704067199;
    esp_base_time_poll();
    assert(!esp_base_time_ready());
    wall_seconds = 1704067200;
    esp_base_time_poll();
    assert(esp_base_time_ready());
    wait_result = ESP_ERR_TIMEOUT;
    wall_seconds += 3600;
    esp_base_time_poll();
    assert(esp_base_time_ready());
    wall_seconds = 1704067199;
    assert(!esp_base_time_ready());
    wall_seconds = 1704067200;
    assert(esp_base_time_ready());
    wait_result = ESP_ERR_INVALID_STATE;
    esp_base_time_poll();
    assert(!esp_base_time_ready());
    wait_result = ESP_OK;
    esp_base_time_poll();
    assert(esp_base_time_ready());

    puts("  time_runtime passed (sync proof, invalid clock, retry, nonblocking poll)");
}
