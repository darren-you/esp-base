// SPDX-License-Identifier: Apache-2.0
#include "base_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip6_addr.h"
#include "lwip/opt.h"
#include "sdkconfig.h"

#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY 1 /* 只高于空闲任务：采样不和控制、联网、下载抢 CPU */

static const char *TAG = "base_profile";
static base_profile_t s_profile; /* 受 s_lock 保护 */
static base_profile_t s_scratch; /* 只在采样任务里用 */
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static base_profile_ready_cb_t s_ready;

static void copy(char *out, size_t size, const char *value)
{
    snprintf(out, size, "%s", value ? value : "");
}

static void format_mac(char out[18], const uint8_t mac[6])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* 启动后不会变的信息。 */
static void collect_static(base_profile_t *p)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    copy(p->chip_model, sizeof p->chip_model, CONFIG_IDF_TARGET);
    snprintf(p->chip_revision, sizeof p->chip_revision, "v%u.%u", (unsigned)(chip.revision / 100),
             (unsigned)(chip.revision % 100));
    p->cores = chip.cores;
    p->cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    static const struct { uint32_t idf, base; } features[] = {
        {CHIP_FEATURE_WIFI_BGN, BASE_CHIP_WIFI}, {CHIP_FEATURE_BLE, BASE_CHIP_BLE}, {CHIP_FEATURE_BT, BASE_CHIP_BT},
        {CHIP_FEATURE_IEEE802154, BASE_CHIP_IEEE802154}, {CHIP_FEATURE_EMB_FLASH, BASE_CHIP_EMB_FLASH},
        {CHIP_FEATURE_EMB_PSRAM, BASE_CHIP_EMB_PSRAM},
    };
    for (size_t i = 0; i < sizeof features / sizeof *features; ++i)
        if (chip.features & features[i].idf) p->chip_features |= features[i].base;
    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK) p->flash_bytes = flash;
    p->psram_bytes = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    copy(p->idf, sizeof p->idf, esp_get_idf_version());
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) format_mac(p->mac, mac);
}

/* 各任务栈的历史最小余量。同名任务（如系统与业务两条 MQTT 连接的 mqtt_task）加序号，避免重复的键。 */
static void sample_stacks(base_profile_t *p)
{
    p->stack_count = 0;
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *tasks = malloc(capacity * sizeof *tasks);
    const UBaseType_t count = tasks ? uxTaskGetSystemState(tasks, capacity, NULL) : 0;
    for (UBaseType_t i = 0; i < count && p->stack_count < BASE_STACK_SLOTS; ++i) {
        char name[configMAX_TASK_NAME_LEN + 1] = {0};
        for (size_t c = 0; c < configMAX_TASK_NAME_LEN && tasks[i].pcTaskName[c]; ++c) {
            const char ch = tasks[i].pcTaskName[c];
            name[c] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? ch : '_';
        }
        unsigned same = 0;
        for (UBaseType_t j = 0; j < i; ++j)
            same += !strncmp(tasks[j].pcTaskName, tasks[i].pcTaskName, configMAX_TASK_NAME_LEN);
        char suffix[8] = "";
        if (same) snprintf(suffix, sizeof suffix, "_%u", same + 1);
        snprintf(p->stack[p->stack_count].name, BASE_TASK_NAME_BYTES, "%s%s", name, suffix);
        p->stack[p->stack_count].bytes = (uint32_t)tasks[i].usStackHighWaterMark;
        ++p->stack_count;
    }
    free(tasks);
}

static void sample_wifi(base_profile_t *p)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        p->rssi = 0;
        p->ssid[0] = p->bssid[0] = '\0';
        p->channel = 0;
        return;
    }
    p->rssi = ap.rssi;
    copy(p->ssid, sizeof p->ssid, (const char *)ap.ssid);
    format_mac(p->bssid, ap.bssid);
    p->channel = ap.primary;
}

static void sample_netif(base_profile_t *p)
{
    p->hostname[0] = '\0';
    p->has_ipv4 = false;
    p->ipv6_count = 0;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;
    const char *hostname = NULL;
    if (esp_netif_get_hostname(netif, &hostname) == ESP_OK) copy(p->hostname, sizeof p->hostname, hostname);
    esp_netif_ip_info_t ip;
    if (esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        p->has_ipv4 = true;
        snprintf(p->ipv4, sizeof p->ipv4, IPSTR, IP2STR(&ip.ip));
        snprintf(p->netmask, sizeof p->netmask, IPSTR, IP2STR(&ip.netmask));
        snprintf(p->gateway, sizeof p->gateway, IPSTR, IP2STR(&ip.gw));
    }
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t addresses[LWIP_IPV6_NUM_ADDRESSES];
    const int count = esp_netif_get_all_ip6(netif, addresses);
    for (int i = 0; i < count && p->ipv6_count < BASE_IPV6_SLOTS; ++i) {
        /* esp_ip6_addr_t 与 lwIP 的 ip6_addr_t 布局相同；用 lwIP 写出压缩格式。 */
        if (ip6addr_ntoa_r((const ip6_addr_t *)&addresses[i], p->ipv6[p->ipv6_count], BASE_IP_TEXT_BYTES))
            ++p->ipv6_count;
    }
#endif
}

/* 采样结果只覆盖运行字段，不动身份、芯片和设置。 */
static void store_runtime(base_profile_t *dst, const base_profile_t *src)
{
    dst->uptime_s = src->uptime_s;
    dst->heap_free = src->heap_free;
    dst->heap_min = src->heap_min;
    dst->heap_largest = src->heap_largest;
    memcpy(dst->stack, src->stack, sizeof dst->stack);
    dst->stack_count = src->stack_count;
    dst->rssi = src->rssi;
    memcpy(dst->ssid, src->ssid, sizeof dst->ssid);
    memcpy(dst->bssid, src->bssid, sizeof dst->bssid);
    dst->channel = src->channel;
    memcpy(dst->hostname, src->hostname, sizeof dst->hostname);
    dst->has_ipv4 = src->has_ipv4;
    memcpy(dst->ipv4, src->ipv4, sizeof dst->ipv4);
    memcpy(dst->netmask, src->netmask, sizeof dst->netmask);
    memcpy(dst->gateway, src->gateway, sizeof dst->gateway);
    memcpy(dst->ipv6, src->ipv6, sizeof dst->ipv6);
    dst->ipv6_count = src->ipv6_count;
}

static void sampler(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* 采样期间的多次请求合并为下一次 */
        base_profile_t *p = &s_scratch;
        p->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        p->heap_free = esp_get_free_heap_size();
        p->heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        p->heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        sample_stacks(p);
        sample_wifi(p);
        sample_netif(p);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        store_runtime(&s_profile, p);
        xSemaphoreGive(s_lock);
        if (s_ready) s_ready();
    }
}

esp_err_t base_profile_start(const char *device_id, const char *boot_id, const char *reset,
                             const base_fw_t *fw, base_profile_ready_cb_t ready)
{
    if (s_lock || !device_id || !boot_id || !fw) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    copy(s_profile.device_id, sizeof s_profile.device_id, device_id);
    copy(s_profile.boot_id, sizeof s_profile.boot_id, boot_id);
    copy(s_profile.reset, sizeof s_profile.reset, reset);
    s_profile.fw = *fw;
    collect_static(&s_profile);
    s_ready = ready;
    if (xTaskCreate(sampler, "profile", TASK_STACK_BYTES, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "sampler task unavailable");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "chip=%s %s cores=%u flash=%u psram=%u mac=%s idf=%s", s_profile.chip_model, s_profile.chip_revision,
             (unsigned)s_profile.cores, (unsigned)s_profile.flash_bytes, (unsigned)s_profile.psram_bytes, s_profile.mac,
             s_profile.idf);
    return ESP_OK;
}

void base_profile_set(const base_profile_settings_t *settings)
{
    if (!s_lock || !settings) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy(s_profile.name, sizeof s_profile.name, settings->name);
    copy(s_profile.group, sizeof s_profile.group, settings->group);
    copy(s_profile.mqtt_uri, sizeof s_profile.mqtt_uri, settings->mqtt_uri);
    copy(s_profile.business_mqtt_uri, sizeof s_profile.business_mqtt_uri, settings->business_mqtt_uri);
    s_profile.config_revision = settings->config_revision;
    copy(s_profile.config_error, sizeof s_profile.config_error, settings->config_error);
    s_profile.wakeups = settings->wakeups;
    xSemaphoreGive(s_lock);
}

void base_profile_request(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

bool base_profile_get(base_profile_t *out)
{
    if (!s_lock || !out) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_profile;
    xSemaphoreGive(s_lock);
    return true;
}

static int encode(int (*encoder)(const base_profile_t *, char *, size_t), char *out, size_t size)
{
    base_profile_t *snapshot = malloc(sizeof *snapshot);
    const int n = snapshot && base_profile_get(snapshot) ? encoder(snapshot, out, size) : -1;
    free(snapshot);
    return n;
}

int base_profile_status(char *out, size_t size)
{
    return encode(base_status_encode, out, size);
}

int base_profile_will(char *out, size_t size)
{
    return encode(base_will_encode, out, size);
}
