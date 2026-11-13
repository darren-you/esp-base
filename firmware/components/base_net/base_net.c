// SPDX-License-Identifier: Apache-2.0
#include "base_net.h"

#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#define RECONNECT_MIN_MS 1000u
#define RECONNECT_MAX_MS 30000u
#define VALID_UNIX_SECONDS ((time_t)1704067200) /* 2024-01-01 UTC */

static const char *TAG = "base_net";
static base_net_cb_t s_callback;
static esp_timer_handle_t s_reconnect;
static uint32_t s_delay_ms = RECONNECT_MIN_MS;
static bool s_up, s_sntp_started;

static void reconnect(void *argument)
{
    (void)argument;
    esp_wifi_connect();
}

static void on_time(struct timeval *tv)
{
    (void)tv;
    s_callback(BASE_NET_TIME);
}

static void on_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "wifi disconnected reason=%d retry_ms=%u", event->reason, (unsigned)s_delay_ms);
        if (s_up) {
            s_up = false;
            s_callback(BASE_NET_DOWN);
        }
        esp_timer_stop(s_reconnect);
        esp_timer_start_once(s_reconnect, (uint64_t)s_delay_ms * 1000u);
        s_delay_ms = s_delay_ms * 2u > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : s_delay_ms * 2u;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_delay_ms = RECONNECT_MIN_MS;
        s_up = true;
        if (!s_sntp_started) s_sntp_started = esp_netif_sntp_start() == ESP_OK;
        s_callback(BASE_NET_UP);
    }
}

static esp_err_t set_credentials(const char *ssid, const char *password)
{
    wifi_config_t config = {0};
    if (!ssid || !password || strlen(ssid) >= sizeof config.sta.ssid ||
        strlen(password) >= sizeof config.sta.password) return ESP_ERR_INVALID_ARG;
    strcpy((char *)config.sta.ssid, ssid);
    strcpy((char *)config.sta.password, password);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

esp_err_t base_net_start(const char *ssid, const char *password, base_net_cb_t callback)
{
    if (!callback || s_callback) return ESP_ERR_INVALID_STATE;
    s_callback = callback;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t timer = {.callback = reconnect, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s_reconnect), TAG, "timer");
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "handler");
    /* 凭据只在 base 命名空间保存一份；省电模式保持 IDF 默认值。 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(set_credentials(ssid, password), TAG, "credentials");
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    sntp.start = false;
    sntp.wait_for_sync = false;
    sntp.sync_cb = on_time;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp), TAG, "sntp");
    return esp_wifi_start();
}

esp_err_t base_net_reconfigure(const char *ssid, const char *password)
{
    ESP_RETURN_ON_ERROR(set_credentials(ssid, password), TAG, "credentials");
    s_delay_ms = RECONNECT_MIN_MS;
    /* 断开事件会按最短退避用新凭据重连；本来就没连上时直接连接。 */
    if (esp_wifi_disconnect() != ESP_OK || !s_up) {
        esp_timer_stop(s_reconnect);
        esp_timer_start_once(s_reconnect, (uint64_t)RECONNECT_MIN_MS * 1000u);
    }
    return ESP_OK;
}

bool base_net_time_valid(void)
{
    return time(NULL) >= VALID_UNIX_SECONDS;
}

int base_net_rssi(void)
{
    wifi_ap_record_t ap = {0};
    return s_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
