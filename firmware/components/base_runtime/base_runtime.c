// SPDX-License-Identifier: Apache-2.0
// 业务运行时：登记业务的任务和连接，把它们与系统层分开；业务的状态由系统层随 Profile 上报。
#include "base_runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "base_runtime_system.h"
#include "base_store.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

_Static_assert(BASE_RUNTIME_PRIORITY_MAX < BASE_SYSTEM_TASK_PRIORITY, "business tasks must stay below system tasks");

#define TASK_SLOTS 4
_Static_assert(TASK_SLOTS + BASE_LINK_SLOTS <= BASE_BUSINESS_TASK_SLOTS, "every business task must fit the profile");
#define FAULT_MAGIC 0x62a7f00du

typedef struct {
    TaskHandle_t handle; /* 空：槽位空闲 */
    void (*entry)(void *);
    void *argument;
    esp_timer_handle_t watchdog;
    uint32_t watchdog_ms;
} task_slot_t;

typedef struct {
    esp_mqtt_client_handle_t client; /* 空：槽位空闲 */
    char name[BASE_LINK_NAME_BYTES];
    char uri[BASE_URI_BYTES];
    bool connected;
    TaskHandle_t task; /* 这条连接的 esp-mqtt 任务，任务启动后的第一个事件里记下 */
} link_slot_t;

static const char *TAG = "base_runtime";
/* 运行时因业务看门狗超时重启设备时留下的标记；软件重启不清 RTC 内存，下次启动由系统读取。 */
static RTC_NOINIT_ATTR uint32_t s_fault;
static SemaphoreHandle_t s_lock;
static const base_business_t *s_business;
static bool s_running, s_abnormal;
static char s_error[BASE_ERROR_BYTES];
static char s_device_id[BASE_UUID_BYTES];
static base_mqtt_t s_mqtt;
static base_runtime_changed_cb_t s_changed, s_app_changed;
static atomic_bool s_updating;
static task_slot_t s_tasks[TASK_SLOTS];
static link_slot_t s_links[BASE_LINK_SLOTS];

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ---------- 留给系统层的接口 ---------- */

bool base_runtime_take_fault(void)
{
    const bool fault = esp_reset_reason() == ESP_RST_SW && s_fault == FAULT_MAGIC;
    s_fault = 0;
    return fault;
}

void base_runtime_boot(const base_business_t *business, const char *device_id, const base_mqtt_t *mqtt, bool enabled,
                       const char *error, bool abnormal, base_runtime_changed_cb_t changed,
                       base_runtime_changed_cb_t app_changed)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_abnormal = abnormal;
    snprintf(s_device_id, sizeof s_device_id, "%s", device_id ? device_id : "");
    if (mqtt) s_mqtt = *mqtt;
    s_changed = changed;
    s_app_changed = app_changed;
    if (!s_lock || !business) return;
    s_business = business;
    if (!base_is_identifier(business->name, BASE_BUSINESS_NAME_BYTES)) {
        /* 固件自己写错了业务名：不运行业务，上报原因。 */
        enabled = false;
        error = "invalid_name";
    }
    s_running = enabled;
    snprintf(s_error, sizeof s_error, "%s", !enabled && error ? error : "");
    if (!enabled) {
        ESP_LOGE(TAG, "ESP_BASE_BUSINESS_DISABLED reason=%s; the system keeps running", s_error);
        return;
    }
    ESP_LOGI(TAG, "ESP_BASE_BUSINESS name=%s", business->name);
    if (business->init) business->init();
}

bool base_runtime_active(void)
{
    return s_running;
}

void base_runtime_set_mqtt(const base_mqtt_t *mqtt)
{
    if (!s_lock || !mqtt) return;
    lock();
    s_mqtt = *mqtt;
    unlock();
}

void base_runtime_connect(void)
{
    if (s_running && s_business->connect) s_business->connect();
}

void base_runtime_deploy(const char *payload, size_t length)
{
    if (s_running && s_business->deploy) s_business->deploy(payload, length);
}

int base_runtime_app_state(char *out, size_t size)
{
    return s_running && s_business->state ? s_business->state(out, size) : 0;
}

void base_runtime_set_updating(bool updating)
{
    atomic_store(&s_updating, updating);
}

bool base_runtime_owns_task(TaskHandle_t task)
{
    if (!s_lock || !task) return false;
    bool owned = false;
    lock();
    for (int i = 0; i < TASK_SLOTS && !owned; ++i) owned = s_tasks[i].handle == task;
    for (int i = 0; i < BASE_LINK_SLOTS && !owned; ++i) owned = s_links[i].client && s_links[i].task == task;
    unlock();
    return owned;
}

void base_runtime_snapshot(base_business_profile_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s_lock || !s_business) return;
    out->present = true;
    snprintf(out->name, sizeof out->name, "%s", base_is_identifier(s_business->name, sizeof out->name) ? s_business->name : "unnamed");
    out->running = s_running;
    snprintf(out->error, sizeof out->error, "%s", s_error);
    lock();
    base_public_uri(out->mqtt_uri, s_mqtt.uri);
    for (int i = 0; i < TASK_SLOTS; ++i) {
        if (!s_tasks[i].handle) continue;
        base_task_stack_add(out->tasks, &out->task_count, BASE_BUSINESS_TASK_SLOTS, pcTaskGetName(s_tasks[i].handle),
                            (uint32_t)uxTaskGetStackHighWaterMark(s_tasks[i].handle));
    }
    for (int i = 0; i < BASE_LINK_SLOTS; ++i) {
        const link_slot_t *link = &s_links[i];
        if (!link->client) continue;
        snprintf(out->links[out->link_count].name, BASE_LINK_NAME_BYTES, "%s", link->name);
        snprintf(out->links[out->link_count].uri, BASE_URI_BYTES, "%s", link->uri);
        out->links[out->link_count].connected = link->connected;
        ++out->link_count;
        if (link->task)
            base_task_stack_add(out->tasks, &out->task_count, BASE_BUSINESS_TASK_SLOTS, pcTaskGetName(link->task),
                                (uint32_t)uxTaskGetStackHighWaterMark(link->task));
    }
    unlock();
}

/* ---------- 业务接口 ---------- */

void base_runtime_app_changed(void)
{
    if (s_app_changed) s_app_changed();
}

bool base_runtime_system_updating(void)
{
    return atomic_load(&s_updating);
}

bool base_runtime_abnormal_restart(void)
{
    return s_abnormal;
}

const char *base_runtime_device_id(void)
{
    return s_device_id;
}

esp_err_t base_runtime_param(const char *key, char *out, size_t size)
{
    if (!key || !out || size == 0) return ESP_ERR_INVALID_ARG;
    return base_store_business_string(key, out, size);
}

bool base_runtime_mqtt_config(base_runtime_mqtt_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!s_lock) return false;
    lock();
    snprintf(out->uri, sizeof out->uri, "%s", s_mqtt.uri);
    snprintf(out->username, sizeof out->username, "%s", s_mqtt.username);
    snprintf(out->password, sizeof out->password, "%s", s_mqtt.password);
    unlock();
    return out->uri[0] != '\0';
}

static void watchdog_expired(void *argument)
{
    const task_slot_t *slot = argument;
    ESP_LOGE(TAG, "ESP_BASE_BUSINESS_WATCHDOG task=%s silent for %u ms; restarting", pcTaskGetName(slot->handle),
             (unsigned)slot->watchdog_ms);
    /* 在定时器任务里：不写 NVS，只留标记，下次启动由系统计入业务守卫。 */
    s_fault = FAULT_MAGIC;
    esp_restart();
}

static void release_task(task_slot_t *slot)
{
    lock();
    if (slot->watchdog) {
        esp_timer_stop(slot->watchdog);
        esp_timer_delete(slot->watchdog);
    }
    memset(slot, 0, sizeof *slot);
    unlock();
}

static void task_main(void *argument)
{
    task_slot_t *slot = argument;
    /* 等创建方登记完句柄：之后 base_runtime_feed 才认得这个任务。 */
    lock();
    unlock();
    slot->entry(slot->argument);
    release_task(slot);
    vTaskDelete(NULL);
}

esp_err_t base_runtime_task(const char *name, void (*entry)(void *argument), void *argument, uint32_t stack_bytes,
                            unsigned priority, uint32_t watchdog_ms)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if (!name || !entry || stack_bytes == 0) return ESP_ERR_INVALID_ARG;
    if (priority < 1) priority = 1;
    if (priority > BASE_RUNTIME_PRIORITY_MAX) priority = BASE_RUNTIME_PRIORITY_MAX;
    esp_err_t result = ESP_ERR_NO_MEM;
    lock();
    task_slot_t *slot = NULL;
    for (int i = 0; i < TASK_SLOTS && !slot; ++i)
        if (!s_tasks[i].entry) slot = &s_tasks[i];
    if (slot) {
        *slot = (task_slot_t){.entry = entry, .argument = argument, .watchdog_ms = watchdog_ms};
        const esp_timer_create_args_t watchdog = {.callback = watchdog_expired, .arg = slot, .name = "biz_wdt"};
        if ((watchdog_ms == 0 || esp_timer_create(&watchdog, &slot->watchdog) == ESP_OK) &&
            xTaskCreate(task_main, name, stack_bytes, slot, priority, &slot->handle) == pdPASS) {
            if (slot->watchdog) esp_timer_start_once(slot->watchdog, (uint64_t)watchdog_ms * 1000u);
            result = ESP_OK;
        } else {
            if (slot->watchdog) esp_timer_delete(slot->watchdog);
            memset(slot, 0, sizeof *slot);
        }
    }
    unlock();
    if (result != ESP_OK) ESP_LOGE(TAG, "business task %s not created", name);
    return result;
}

void base_runtime_feed(void)
{
    if (!s_lock) return;
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    lock();
    for (int i = 0; i < TASK_SLOTS; ++i) {
        if (s_tasks[i].handle != current || !s_tasks[i].watchdog) continue;
        esp_timer_restart(s_tasks[i].watchdog, (uint64_t)s_tasks[i].watchdog_ms * 1000u);
        break;
    }
    unlock();
}

/* 运行时自己的事件处理：只记连接状态，排在业务注册的处理函数之前。 */
static void link_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    link_slot_t *link = argument;
    const esp_mqtt_event_handle_t event = data;
    if (id != MQTT_EVENT_BEFORE_CONNECT && id != MQTT_EVENT_CONNECTED && id != MQTT_EVENT_DISCONNECTED) return;
    bool changed = false;
    lock();
    if (link->client == event->client) { /* 槽位可能已经关闭或换了连接 */
        /* BEFORE_CONNECT 只在 esp-mqtt 自己的任务里发出：记下这条连接的任务。 */
        if (id == MQTT_EVENT_BEFORE_CONNECT) link->task = xTaskGetCurrentTaskHandle();
        const bool connected = id == MQTT_EVENT_CONNECTED ? true : id == MQTT_EVENT_DISCONNECTED ? false : link->connected;
        changed = connected != link->connected;
        link->connected = connected;
    }
    unlock();
    if (changed && s_changed) s_changed();
}

/* 上报用的地址：优先 uri，否则用 hostname、port 和 transport 拼出来。 */
static void link_uri(char out[BASE_URI_BYTES], const esp_mqtt_client_config_t *config)
{
    if (config->broker.address.uri) {
        base_public_uri(out, config->broker.address.uri);
        return;
    }
    static const char *const schemes[] = {[MQTT_TRANSPORT_OVER_TCP] = "mqtt", [MQTT_TRANSPORT_OVER_SSL] = "mqtts",
                                          [MQTT_TRANSPORT_OVER_WS] = "ws", [MQTT_TRANSPORT_OVER_WSS] = "wss"};
    const unsigned transport = (unsigned)config->broker.address.transport;
    const char *scheme = transport < sizeof schemes / sizeof *schemes ? schemes[transport] : NULL;
    char uri[BASE_URI_BYTES];
    const int n = snprintf(uri, sizeof uri, "%s://%s:%u", scheme ? scheme : "mqtt",
                           config->broker.address.hostname ? config->broker.address.hostname : "",
                           (unsigned)config->broker.address.port);
    if (n > 0 && (size_t)n < sizeof uri) base_public_uri(out, uri);
    else out[0] = '\0';
}

esp_mqtt_client_handle_t base_runtime_mqtt_open(const char *name, const esp_mqtt_client_config_t *config)
{
    if (!s_running || !config || !base_is_identifier(name, BASE_LINK_NAME_BYTES)) return NULL;
    esp_mqtt_client_config_t effective = *config;
    if (effective.task.priority <= 0 || effective.task.priority > BASE_RUNTIME_PRIORITY_MAX)
        effective.task.priority = BASE_RUNTIME_PRIORITY_MAX;
    lock();
    link_slot_t *link = NULL;
    for (int i = 0; i < BASE_LINK_SLOTS && !link; ++i)
        if (!s_links[i].client) link = &s_links[i];
    esp_mqtt_client_handle_t client = link ? esp_mqtt_client_init(&effective) : NULL;
    if (client && esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, link_event, link) != ESP_OK) {
        esp_mqtt_client_destroy(client);
        client = NULL;
    }
    if (client) {
        memset(link, 0, sizeof *link);
        link->client = client;
        snprintf(link->name, sizeof link->name, "%s", name);
        link_uri(link->uri, config);
    }
    unlock();
    if (!client) ESP_LOGE(TAG, "business link %s not opened", name);
    else if (s_changed) s_changed();
    return client;
}

void base_runtime_mqtt_close(esp_mqtt_client_handle_t client)
{
    if (!s_lock || !client) return;
    bool found = false;
    lock();
    for (int i = 0; i < BASE_LINK_SLOTS && !found; ++i) {
        if (s_links[i].client != client) continue;
        memset(&s_links[i], 0, sizeof s_links[i]); /* 先注销：之后的事件和采样都不再碰这条连接的任务 */
        found = true;
    }
    unlock();
    if (!found) return;
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    if (s_changed) s_changed();
}
