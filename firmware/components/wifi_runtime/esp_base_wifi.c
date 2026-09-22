// SPDX-License-Identifier: Apache-2.0
#include "esp_base_wifi.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

enum { STARTED, STOPPED, ASSOCIATED, DISCONNECTED, GOT_ADDRESS, LOST_ADDRESS };
static QueueHandle_t s_events;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_handler, s_ip_handler;
static atomic_bool s_overflow;
static ebase_wifi_config_t s_config;
static bool s_initialized, s_active, s_stopping, s_associated, s_ready, s_connecting;
static uint64_t s_retry_at, s_stop_deadline;
static unsigned s_failures;
static const char *s_state = "unconfigured";

static void event_received(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    unsigned event;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: event = STARTED; break;
        case WIFI_EVENT_STA_STOP: event = STOPPED; break;
        case WIFI_EVENT_STA_CONNECTED: event = ASSOCIATED; break;
        case WIFI_EVENT_STA_DISCONNECTED: event = DISCONNECTED; break;
        default: return;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) event = GOT_ADDRESS;
    else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) event = LOST_ADDRESS;
    else return;
    if (xQueueSend(s_events, &event, 0) != pdTRUE) atomic_store(&s_overflow, true);
}

static void failed(void)
{
    s_state = "failed";
    s_ready = s_associated = s_connecting = false;
    s_retry_at = 0;
}

static esp_err_t start_selected(void)
{
    s_ready = s_associated = s_connecting = false;
    s_retry_at = 0;
    if (!s_config.configured) { s_state = "unconfigured"; return ESP_OK; }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, s_config.ssid, strlen(s_config.ssid));
    memcpy(config.sta.password, s_config.password, strlen(s_config.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof config);
    if (error == ESP_OK) error = esp_wifi_start();
    if (error != ESP_OK) { failed(); return error; }
    s_active = true;
    s_state = "connecting";
    return ESP_OK;
}

esp_err_t esp_base_wifi_apply(const ebase_wifi_config_t *config, uint64_t now)
{
    if (!s_initialized || !config) return ESP_ERR_INVALID_STATE;
    esp_base_remote_config_t checked = {.wifi = *config};
    if (!ebase_config_valid(&checked)) return ESP_ERR_INVALID_ARG;
    s_config = *config;
    s_ready = s_associated = s_connecting = false;
    s_retry_at = 0; s_failures = 0;
    if (s_stopping) return ESP_OK; /* Latest explicit selection starts after STOP. */
    if (!s_active) return start_selected();
    s_stopping = true;
    s_stop_deadline = now + 2000;
    s_state = "connecting";
    esp_err_t error = esp_wifi_stop();
    if (error == ESP_ERR_WIFI_NOT_STARTED) {
        s_stopping = s_active = false;
        return start_selected();
    }
    if (error != ESP_OK) { s_stopping = false; failed(); }
    return error;
}

static void schedule_retry(uint64_t now)
{
    s_ready = s_associated = s_connecting = false;
    s_state = "disconnected";
    if (s_failures < 5) ++s_failures;
    uint64_t delay = (uint64_t)1000 << (s_failures ? s_failures - 1 : 0);
    s_retry_at = now + delay + esp_random() % 500;
}

void esp_base_wifi_poll(uint64_t now)
{
    if (!s_initialized) return;
    if (atomic_exchange(&s_overflow, false)) { failed(); return; }
    unsigned event;
    while (xQueueReceive(s_events, &event, 0) == pdTRUE) {
        if (event == STOPPED) {
            s_active = s_stopping = false;
            s_ready = s_associated = s_connecting = false;
            if (strcmp(s_state, "failed")) (void)start_selected();
            continue;
        }
        if (!s_active || s_stopping || !strcmp(s_state, "failed")) continue;
        if (event == STARTED) s_retry_at = now;
        else if (event == ASSOCIATED) s_associated = true;
        else if (event == DISCONNECTED) schedule_retry(now);
        else if (event == LOST_ADDRESS) {
            /* DHCP loss invalidates proof; reconnect through the same owner. */
            s_ready = false;
            if (esp_wifi_disconnect() != ESP_OK) schedule_retry(now);
        } else if (event == GOT_ADDRESS && s_associated) {
            wifi_ap_record_t ap = {0};
            esp_netif_ip_info_t ip = {0};
            /* Recheck current driver/netif facts so an old queued IP event can
             * never commit credentials for a different association. */
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
                esp_netif_is_netif_up(s_netif) && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK &&
                ip.ip.addr != 0 && !memcmp(ap.ssid, s_config.ssid, 32) && (ap.authmode == WIFI_AUTH_WPA2_PSK || ap.authmode == WIFI_AUTH_WPA_WPA2_PSK ||
                ap.authmode == WIFI_AUTH_WPA3_PSK || ap.authmode == WIFI_AUTH_WPA2_WPA3_PSK)) {
                s_ready = true; s_connecting = false; s_retry_at = 0; s_failures = 0;
                s_state = "connected";
            }
        }
    }
    if (s_stopping && now >= s_stop_deadline) { failed(); return; }
    if (s_active && !s_stopping && s_retry_at && now >= s_retry_at && !s_connecting) {
        s_retry_at = 0;
        if (esp_wifi_connect() == ESP_OK) { s_connecting = true; s_state = "connecting"; }
        else schedule_retry(now);
    }
}

bool esp_base_wifi_ready(void) { return s_ready && !s_stopping; }
const char *esp_base_wifi_state(void) { return s_state; }

esp_err_t esp_base_wifi_start(const ebase_wifi_config_t *config)
{
    if (s_initialized || !config) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK) return error;
    s_events = xQueueCreate(16, sizeof(unsigned));
    if (!s_events) { esp_event_loop_delete_default(); return ESP_ERR_NO_MEM; }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { vQueueDelete(s_events); esp_event_loop_delete_default(); return ESP_ERR_NO_MEM; }
    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    initialization.nvs_enable = false;
    error = esp_wifi_init(&initialization);
    if (error != ESP_OK) goto release_netif;
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) goto release_driver;
    error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_received, NULL, &s_wifi_handler);
    if (error != ESP_OK) goto release_driver;
    error = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event_received, NULL, &s_ip_handler);
    if (error != ESP_OK) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler); goto release_driver; }
    s_initialized = true;
    return esp_base_wifi_apply(config, (uint64_t)(esp_timer_get_time() / 1000));
release_driver:
    esp_wifi_deinit();
release_netif:
    esp_netif_destroy_default_wifi(s_netif);
    vQueueDelete(s_events);
    esp_event_loop_delete_default();
    return error;
}
