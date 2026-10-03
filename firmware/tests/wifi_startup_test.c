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
static unsigned connect_calls, config_calls, start_calls, stop_calls;
static esp_err_t stop_result = ESP_OK;
static wifi_config_t selected_driver_config;
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
    assert(config->sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
    ++config_calls;
    selected_driver_config = *config;
    return result_for(WIFI_CONFIG_FAILURE);
}
esp_err_t esp_wifi_start(void)
{ assert(live_wifi == 1); ++start_calls; return result_for(WIFI_START_FAILURE); }
esp_err_t esp_wifi_stop(void) { assert(live_wifi == 1); ++stop_calls; return stop_result; }
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

static void prove_address(const char *ssid, uint64_t now)
{
    ap_ssid = ssid;
    emit(WIFI_EVENT, WIFI_EVENT_STA_START);
    emit(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED);
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    esp_base_wifi_poll(now);
    assert(esp_base_wifi_ready() && !strcmp(esp_base_wifi_state(), "connected"));
}

static void check_driver_selection(const ebase_wifi_config_t *config)
{
    wifi_config_t expected = {0};
    memcpy(expected.sta.ssid, config->ssid, strlen(config->ssid));
    memcpy(expected.sta.password, config->password, strlen(config->password));
    assert(!memcmp(selected_driver_config.sta.ssid, expected.sta.ssid,
                   sizeof expected.sta.ssid));
    assert(!memcmp(selected_driver_config.sta.password, expected.sta.password,
                   sizeof expected.sta.password));
}

static void lifecycle(const ebase_wifi_config_t *original)
{
    const unsigned initial_config = config_calls, initial_start = start_calls;
    const unsigned initial_stop = stop_calls;
    /* Three full device selections model Wi-Fi-only, FRP addition and removal. */
    ebase_wifi_config_t same = *original;
    for (unsigned i = 0; i < 3; ++i) {
        assert(esp_base_wifi_apply(&same, 1100 + i) == ESP_OK);
        esp_base_wifi_poll(1100 + i);
        assert(esp_base_wifi_ready() && !strcmp(esp_base_wifi_state(), "connected"));
    }
    assert(config_calls == initial_config && start_calls == initial_start && stop_calls == initial_stop);

    ebase_wifi_config_t invalid = *original;
    invalid.ssid[strlen(invalid.ssid) + 1] = 'x';
    assert(esp_base_wifi_apply(&invalid, 1199) == ESP_ERR_INVALID_ARG);
    invalid = *original;
    memset(invalid.ssid, 'x', sizeof invalid.ssid);
    assert(esp_base_wifi_apply(&invalid, 1200) == ESP_ERR_INVALID_ARG);
    invalid = *original; invalid.configured = false;
    assert(esp_base_wifi_apply(&invalid, 1201) == ESP_ERR_INVALID_ARG);
    assert(esp_base_wifi_apply(NULL, 1202) == ESP_ERR_INVALID_STATE);
    assert(esp_base_wifi_ready() && stop_calls == initial_stop);

    /* Keep a scheduled retry and its growing backoff, without inventing IP. */
    emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    esp_base_wifi_poll(2000);
    unsigned before_connect = connect_calls;
    assert(esp_base_wifi_apply(original, 2001) == ESP_OK);
    assert(esp_base_wifi_apply(original, 2500) == ESP_OK);
    assert(!esp_base_wifi_ready() && !strcmp(esp_base_wifi_state(), "disconnected"));
    esp_base_wifi_poll(3000); assert(connect_calls == before_connect);
    esp_base_wifi_poll(3001); assert(connect_calls == before_connect + 1);
    assert(esp_base_wifi_apply(original, 3002) == ESP_OK);
    esp_base_wifi_poll(3003);
    assert(connect_calls == before_connect + 1 && !esp_base_wifi_ready());
    emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    esp_base_wifi_poll(3100);
    assert(esp_base_wifi_apply(original, 3101) == ESP_OK);
    esp_base_wifi_poll(5100); assert(connect_calls == before_connect + 1);
    esp_base_wifi_poll(5101); assert(connect_calls == before_connect + 2);
    assert(stop_calls == initial_stop && config_calls == initial_config && start_calls == initial_start);
    prove_address(original->ssid, 5102);

    /* Password-only selection must stop, then configure/start after STOP. */
    ebase_wifi_config_t changed = *original;
    strcpy(changed.password, "87654321");
    assert(esp_base_wifi_apply(&changed, 6000) == ESP_OK);
    assert(stop_calls == initial_stop + 1 && !esp_base_wifi_ready());
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    esp_base_wifi_poll(6001);
    assert(config_calls == initial_config && start_calls == initial_start && !esp_base_wifi_ready());
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(6002);
    assert(config_calls == initial_config + 1 && start_calls == initial_start + 1);
    check_driver_selection(&changed);
    prove_address(changed.ssid, 6003);

    /* A pending stop accepts the latest full selection, including same apply.
     * Old queued IP must neither restart early nor prove the new SSID. */
    strcpy(changed.ssid, "new-wifi");
    assert(esp_base_wifi_apply(&changed, 7000) == ESP_OK);
    assert(esp_base_wifi_apply(&changed, 7001) == ESP_OK);
    ebase_wifi_config_t latest = changed;
    strcpy(latest.ssid, "latest-wifi");
    strcpy(latest.password, "latest-password");
    assert(esp_base_wifi_apply(&latest, 7002) == ESP_OK);
    assert(stop_calls == initial_stop + 2);
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    esp_base_wifi_poll(7003);
    assert(!esp_base_wifi_ready() && config_calls == initial_config + 1);
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(7004);
    assert(config_calls == initial_config + 2 && start_calls == initial_start + 2);
    check_driver_selection(&latest);
    emit(WIFI_EVENT, WIFI_EVENT_STA_START);
    emit(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED);
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    esp_base_wifi_poll(7005);
    assert(!esp_base_wifi_ready()); /* AP still reports the preceding SSID. */
    ap_ssid = latest.ssid;
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    esp_base_wifi_poll(7006);
    assert(esp_base_wifi_ready());

    /* Rollback is a real change; an already selected rollback stays online. */
    assert(esp_base_wifi_apply(original, 8000) == ESP_OK);
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(8001);
    check_driver_selection(original);
    prove_address(original->ssid, 8002);
    unsigned before_stop = stop_calls;
    assert(esp_base_wifi_apply(original, 8003) == ESP_OK);
    assert(esp_base_wifi_ready() && stop_calls == before_stop);

    ebase_wifi_config_t disabled = {0};
    unsigned before_start = start_calls, before_config = config_calls;
    assert(esp_base_wifi_apply(&disabled, 9000) == ESP_OK);
    assert(stop_calls == before_stop + 1 && !esp_base_wifi_ready());
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(9001);
    assert(!strcmp(esp_base_wifi_state(), "unconfigured"));
    assert(esp_base_wifi_apply(&disabled, 9002) == ESP_OK);
    assert(start_calls == before_start && config_calls == before_config && stop_calls == before_stop + 1);
    assert(esp_base_wifi_apply(original, 9003) == ESP_OK);
    assert(start_calls == before_start + 1 && config_calls == before_config + 1);
    assert(esp_base_wifi_apply(original, 9004) == ESP_OK);
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    esp_base_wifi_poll(9005);
    assert(!esp_base_wifi_ready()); /* A fresh start still needs association. */
    prove_address(original->ssid, 9006);

    /* Queue overflow fails the active owner; equal apply must recover it. */
    for (unsigned i = 0; i < 17; ++i) emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    esp_base_wifi_poll(10000);
    assert(!strcmp(esp_base_wifi_state(), "failed") && !esp_base_wifi_ready());
    before_stop = stop_calls;
    assert(esp_base_wifi_apply(original, 10001) == ESP_OK);
    assert(stop_calls == before_stop + 1 && !esp_base_wifi_ready());
    esp_base_wifi_poll(10002); /* Drain old disconnects while stopping. */
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(10003);
    prove_address(original->ssid, 10004);

    stop_result = ESP_FAIL;
    assert(esp_base_wifi_apply(&latest, 11000) == ESP_FAIL);
    assert(!strcmp(esp_base_wifi_state(), "failed") && !esp_base_wifi_ready());
    stop_result = ESP_OK;
    before_stop = stop_calls;
    assert(esp_base_wifi_apply(&latest, 11001) == ESP_OK);
    assert(stop_calls == before_stop + 1);
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(11002);
    check_driver_selection(&latest);
    prove_address(latest.ssid, 11003);

    /* Repeated selection during stopping does not extend the original 2 s. */
    assert(esp_base_wifi_apply(original, 12000) == ESP_OK);
    assert(esp_base_wifi_apply(original, 12500) == ESP_OK);
    esp_base_wifi_poll(13999);
    assert(strcmp(esp_base_wifi_state(), "failed"));
    esp_base_wifi_poll(14000);
    assert(!strcmp(esp_base_wifi_state(), "failed"));
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    esp_base_wifi_poll(14001);
    before_start = start_calls;
    assert(!strcmp(esp_base_wifi_state(), "failed"));
    assert(esp_base_wifi_apply(original, 14002) == ESP_OK);
    assert(start_calls == before_start + 1 && !esp_base_wifi_ready());
    prove_address(original->ssid, 14003);
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
        lifecycle(&config);
    } else if (failed_stage == WIFI_CONFIG_FAILURE || failed_stage == WIFI_START_FAILURE) {
        /* Initialized but never active: an equal selection retries start. */
        unsigned before_config = config_calls;
        failed_stage = NO_FAILURE;
        assert(esp_base_wifi_apply(&config, 1000) == ESP_OK);
        assert(config_calls == before_config + 1 && !esp_base_wifi_ready());
        check_driver_selection(&config);
        prove_address(config.ssid, 1001);
    }
    return 0;
}
