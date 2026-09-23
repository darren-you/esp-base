// SPDX-License-Identifier: Apache-2.0
#include "esp_base_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/queue.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const char *WIFI_EVENT = "wifi";
const char *IP_EVENT = "ip";
static int failed_stage;
static int live_loop, live_queue, live_netif, live_wifi, live_handlers;
static unsigned connect_calls;
static esp_netif_t netif;
static struct fake_queue { unsigned events[16], head, count; } queue;
static esp_event_handler_t wifi_handler, ip_handler;
static const char *ap_ssid = "test-wifi";

enum {
    NO_FAILURE,
    NETIF_INIT_FAILURE,
    EVENT_LOOP_FAILURE,
    QUEUE_FAILURE,
    NETIF_CREATE_FAILURE,
    WIFI_INIT_FAILURE,
    WIFI_STORAGE_FAILURE,
    WIFI_MODE_FAILURE,
    WIFI_HANDLER_FAILURE,
    IP_HANDLER_FAILURE,
    WIFI_CONFIG_FAILURE,
    WIFI_START_FAILURE,
};

static esp_err_t result_for(int stage) { return failed_stage == stage ? ESP_FAIL : ESP_OK; }

esp_err_t esp_netif_init(void) { return result_for(NETIF_INIT_FAILURE); }
esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    if (failed_stage == NETIF_CREATE_FAILURE) return NULL;
    ++live_netif;
    return &netif;
}
void esp_netif_destroy_default_wifi(esp_netif_t *target)
{ assert(target == &netif && live_netif == 1); --live_netif; }
bool esp_netif_is_netif_up(esp_netif_t *target) { assert(target == &netif); return true; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *target, esp_netif_ip_info_t *info)
{ assert(target == &netif); info->ip.addr = 1; return ESP_OK; }

esp_err_t esp_event_loop_create_default(void)
{
    if (failed_stage == EVENT_LOOP_FAILURE) return ESP_FAIL;
    ++live_loop;
    return ESP_OK;
}
esp_err_t esp_event_loop_delete_default(void)
{ assert(live_loop == 1); --live_loop; return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t handler, void *arg, esp_event_handler_instance_t *instance)
{
    assert(id == ESP_EVENT_ANY_ID && handler && !arg);
    if ((base == WIFI_EVENT && failed_stage == WIFI_HANDLER_FAILURE) ||
        (base == IP_EVENT && failed_stage == IP_HANDLER_FAILURE)) return ESP_FAIL;
    ++live_handlers;
    if (base == WIFI_EVENT) wifi_handler = handler;
    else ip_handler = handler;
    *instance = (void *)(uintptr_t)live_handlers;
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance)
{
    assert((base == WIFI_EVENT || base == IP_EVENT) && id == ESP_EVENT_ANY_ID && instance && live_handlers > 0);
    --live_handlers;
    return ESP_OK;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    assert(length == 16 && item_size == sizeof(unsigned));
    if (failed_stage == QUEUE_FAILURE) return NULL;
    ++live_queue;
    return &queue;
}
BaseType_t xQueueSend(QueueHandle_t target, const void *item, TickType_t ticks)
{
    assert(target == &queue && item && ticks == 0);
    if (queue.count == 16) return pdFALSE;
    queue.events[(queue.head + queue.count++) % 16] = *(const unsigned *)item;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t target, void *item, TickType_t ticks)
{
    assert(target == &queue && item && ticks == 0);
    if (!queue.count) return pdFALSE;
    *(unsigned *)item = queue.events[queue.head];
    queue.head = (queue.head + 1) % 16;
    --queue.count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t target)
{ assert(target == &queue && live_queue == 1); --live_queue; }

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    assert(config && !config->nvs_enable);
    if (failed_stage == WIFI_INIT_FAILURE) return ESP_FAIL;
    ++live_wifi;
    return ESP_OK;
}
esp_err_t esp_wifi_deinit(void)
{ assert(live_wifi == 1); --live_wifi; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage)
{ assert(storage == WIFI_STORAGE_RAM && live_wifi == 1); return result_for(WIFI_STORAGE_FAILURE); }
esp_err_t esp_wifi_set_mode(int mode)
{ assert(mode == WIFI_MODE_STA && live_wifi == 1); return result_for(WIFI_MODE_FAILURE); }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{
    assert(interface == WIFI_IF_STA && live_wifi == 1 && config);
    assert(!memcmp(config->sta.ssid, "test-wifi", 9));
    assert(config->sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
    return result_for(WIFI_CONFIG_FAILURE);
}
esp_err_t esp_wifi_start(void)
{ assert(live_wifi == 1); return result_for(WIFI_START_FAILURE); }
esp_err_t esp_wifi_stop(void) { assert(live_wifi == 1); return ESP_OK; }
esp_err_t esp_wifi_connect(void) { assert(live_wifi == 1); ++connect_calls; return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { assert(live_wifi == 1); return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record)
{
    memset(record, 0xa5, sizeof *record);
    memcpy(record->ssid, ap_ssid, strlen(ap_ssid) + 1);
    record->authmode = WIFI_AUTH_WPA2_PSK;
    return ESP_OK;
}
uint32_t esp_random(void) { return 1; }
int64_t esp_timer_get_time(void) { return 1000000; }

static void emit(esp_event_base_t base, int32_t id)
{
    esp_event_handler_t handler = base == WIFI_EVENT ? wifi_handler : ip_handler;
    assert(handler);
    handler(NULL, base, id, NULL);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    failed_stage = atoi(argv[1]);
    assert(failed_stage >= NO_FAILURE && failed_stage <= WIFI_START_FAILURE);
    ebase_wifi_config_t config = {.configured = true, .ssid = "test-wifi", .password = "12345678"};
    const esp_err_t result = esp_base_wifi_start(&config);
    assert(result == (failed_stage == NO_FAILURE ? ESP_OK :
                      failed_stage == QUEUE_FAILURE || failed_stage == NETIF_CREATE_FAILURE ? ESP_ERR_NO_MEM : ESP_FAIL));
    assert(!strcmp(esp_base_wifi_state(), failed_stage == NO_FAILURE ? "connecting" : "failed"));
    assert(!esp_base_wifi_ready());
    if (failed_stage > NO_FAILURE && failed_stage <= IP_HANDLER_FAILURE) {
        assert(!live_loop && !live_queue && !live_netif && !live_wifi && !live_handlers);
        assert(esp_base_wifi_apply(&config, 1000) == ESP_ERR_INVALID_STATE);
    }
    if (failed_stage == NO_FAILURE) {
        emit(WIFI_EVENT, WIFI_EVENT_STA_START);
        esp_base_wifi_poll(1000);
        assert(connect_calls == 1);
        emit(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED);
        ap_ssid = "test-wifi-evil";
        emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
        esp_base_wifi_poll(1001);
        assert(!esp_base_wifi_ready());
        ap_ssid = "test-wifi";
        emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
        esp_base_wifi_poll(1002);
        assert(esp_base_wifi_ready() && !strcmp(esp_base_wifi_state(), "connected"));
    }
    return 0;
}
