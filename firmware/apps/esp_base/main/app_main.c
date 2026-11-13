// SPDX-License-Identifier: Apache-2.0
// ESP Base 主程序：一个控制任务阻塞在事件队列上，Wi-Fi、MQTT、定时器和 OTA 下载任务都只向队列投递事件。
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "base_net.h"
#include "base_ota.h"
#include "base_protocol.h"
#include "base_store.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#define ROLLBACK_WINDOW_MS 300000u      /* 新固件须在此时间内连上 Broker，否则回滚 */
#define CONFIG_TRIAL_MS 120000u         /* 新配置须在此时间内连上 Broker，否则恢复旧配置 */
#define DEFERRED_FALLBACK_MS 3000u      /* 等应答发出的最长时间 */
#define STATUS_REFRESH_MS (15u * 60u * 1000u)
#define MESSAGE_MAX_BYTES 1536u
#define STATUS_BYTES 1280

typedef enum {
    EV_NET_UP, EV_NET_DOWN, EV_NET_TIME,
    EV_MQTT_CONNECTED, EV_MQTT_DISCONNECTED, EV_MQTT_MESSAGE, EV_MQTT_PUBLISHED,
    EV_TIMER, EV_OTA_PROGRESS, EV_OTA_VERIFYING, EV_OTA_DONE,
} event_kind_t;
typedef enum { TIMER_STATUS, TIMER_OTA_RETRY, TIMER_ROLLBACK, TIMER_CONFIG_TRIAL, TIMER_DEFERRED, TIMER_COUNT } timer_id_t;
typedef enum { DEFER_NONE, DEFER_RESTART, DEFER_APPLY_CONFIG } deferred_t;

typedef struct {
    uint8_t kind;
    int32_t value;                 /* 定时器编号、消息 ID、偏移或下载结果 */
    char *message;                 /* EV_MQTT_MESSAGE：topic、NUL、payload、NUL，由控制任务释放 */
    uint16_t topic_len, payload_len;
    char error[BASE_ERROR_BYTES];  /* EV_OTA_DONE */
} event_t;

static const char *TAG = "esp_base";
static QueueHandle_t s_events;
static esp_timer_handle_t s_timers[TIMER_COUNT];
static char s_device_id[BASE_UUID_BYTES], s_boot_id[BASE_UUID_BYTES];
static char s_topics[5][BASE_TOPIC_BYTES];
static char s_will[384];
static const char *s_reset_reason = "other";
static base_config_t s_config, s_trial;
static bool s_trial_active, s_trial_wifi;
static const char *s_config_error;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected, s_pending_verify;
static base_ota_t s_ota;
static base_business_t s_business;
static base_dedupe_t s_dedupe;
static deferred_t s_deferred;
static int s_deferred_message_id;
static uint32_t s_wakeups;

static void post(const event_t *event, TickType_t wait)
{
    if (xQueueSend(s_events, event, wait) != pdTRUE) free(event->message);
}

static void timer_fired(void *argument)
{
    post(&(event_t){.kind = EV_TIMER, .value = (int32_t)(intptr_t)argument}, 0);
}

static void timer_start(timer_id_t id, uint32_t delay_ms)
{
    esp_timer_stop(s_timers[id]);
    esp_timer_start_once(s_timers[id], (uint64_t)delay_ms * 1000u);
}

static void net_event(base_net_event_t event)
{
    static const uint8_t kinds[] = {EV_NET_UP, EV_NET_DOWN, EV_NET_TIME};
    post(&(event_t){.kind = kinds[event]}, 0);
}

static void ota_worker_event(base_ota_worker_event_t event, uint32_t value, const char *error)
{
    event_t posted = {.value = (int32_t)value};
    if (event == BASE_OTA_WORKER_PROGRESS) {
        posted.kind = EV_OTA_PROGRESS;
        post(&posted, 0); /* 队列满时丢弃进度，下一个 64 KiB 边界会再报 */
        return;
    }
    posted.kind = event == BASE_OTA_WORKER_VERIFYING ? EV_OTA_VERIFYING : EV_OTA_DONE;
    if (error) snprintf(posted.error, sizeof posted.error, "%s", error);
    post(&posted, portMAX_DELAY);
}

static void mqtt_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument; (void)base;
    const esp_mqtt_event_handle_t event = data;
    event_t posted = {0};
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: posted.kind = EV_MQTT_CONNECTED; break;
    case MQTT_EVENT_DISCONNECTED: posted.kind = EV_MQTT_DISCONNECTED; break;
    case MQTT_EVENT_PUBLISHED: posted.kind = EV_MQTT_PUBLISHED; posted.value = event->msg_id; break;
    case MQTT_EVENT_DATA:
        /* 协议里的消息都很小；分片到达或超长的一律丢弃。 */
        if (event->current_data_offset != 0 || event->data_len != event->total_data_len ||
            event->topic_len <= 0 || event->topic_len >= BASE_TOPIC_BYTES ||
            (unsigned)event->data_len > MESSAGE_MAX_BYTES) return;
        posted.kind = EV_MQTT_MESSAGE;
        posted.topic_len = (uint16_t)event->topic_len;
        posted.payload_len = (uint16_t)event->data_len;
        posted.message = malloc((size_t)event->topic_len + (size_t)event->data_len + 2u);
        if (!posted.message) return;
        memcpy(posted.message, event->topic, (size_t)event->topic_len);
        posted.message[event->topic_len] = '\0';
        memcpy(posted.message + event->topic_len + 1, event->data, (size_t)event->data_len);
        posted.message[event->topic_len + 1 + event->data_len] = '\0';
        break;
    default: return;
    }
    post(&posted, pdMS_TO_TICKS(1000));
}

static int publish(base_topic_t topic, const char *payload, bool retain)
{
    if (!s_mqtt || !s_mqtt_connected) return -1;
    return esp_mqtt_client_enqueue(s_mqtt, s_topics[topic], payload, (int)strlen(payload), 1, retain, true);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "other";
    }
}

/* 状态文档：固件、启动 ID、堆和各任务栈余量、Wi-Fi 信号、配置与业务。 */
static int status_encode(char *out, size_t size)
{
    int n = snprintf(out, size,
        "{\"online\":true,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"uptime_s\":%" PRIu32 ",\"reset\":\"%s\","
        "\"fw\":{\"version\":\"%s\",\"sha256\":\"%s\",\"target\":\"%s\",\"slot\":\"%s\"},"
        "\"heap\":{\"free\":%" PRIu32 ",\"min\":%u,\"largest\":%u},\"stack\":{",
        s_device_id, s_boot_id, (uint32_t)(esp_timer_get_time() / 1000000), s_reset_reason,
        s_ota.running.version, s_ota.running.sha256, s_ota.running.target, s_ota.running.slot,
        esp_get_free_heap_size(), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    if (n <= 0 || (size_t)n >= size) return -1;
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *tasks = malloc(capacity * sizeof *tasks);
    const UBaseType_t count = tasks ? uxTaskGetSystemState(tasks, capacity, NULL) : 0;
    for (UBaseType_t i = 0; i < count; ++i) {
        char name[configMAX_TASK_NAME_LEN + 1] = {0};
        for (size_t c = 0; c < configMAX_TASK_NAME_LEN && tasks[i].pcTaskName[c]; ++c) {
            const char ch = tasks[i].pcTaskName[c];
            name[c] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? ch : '_';
        }
        const int m = snprintf(out + n, size - (size_t)n, "%s\"%s\":%u", i ? "," : "", name,
                               (unsigned)tasks[i].usStackHighWaterMark);
        if (m <= 0 || (size_t)m >= size - (size_t)n) { free(tasks); return -1; }
        n += m;
    }
    free(tasks);
    char business[48];
    if (base_business_encode(&s_business, business, sizeof business) < 0) return -1;
    char error[BASE_ERROR_BYTES + 2] = "null";
    if (s_config_error) snprintf(error, sizeof error, "\"%s\"", s_config_error);
    const int m = snprintf(out + n, size - (size_t)n,
        "},\"wifi\":{\"rssi\":%d},\"config\":{\"revision\":%" PRIu32 ",\"error\":%s},\"business\":%s,\"wakeups\":%" PRIu32 "}",
        base_net_rssi(), s_config.revision, error, business, s_wakeups);
    return m > 0 && (size_t)m < size - (size_t)n ? n + m : -1;
}

static void publish_status(void)
{
    char *json = malloc(STATUS_BYTES);
    if (json && status_encode(json, STATUS_BYTES) > 0) publish(BASE_TOPIC_STATUS, json, true);
    free(json);
}

static void mqtt_stop(void)
{
    if (!s_mqtt) return;
    esp_mqtt_client_stop(s_mqtt);
    esp_mqtt_client_destroy(s_mqtt);
    s_mqtt = NULL;
    s_mqtt_connected = false;
}

static void mqtt_start(const base_config_t *config)
{
    if (s_mqtt) return;
    const esp_mqtt_client_config_t mqtt = {
        .broker.address.uri = config->mqtt_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = config->mqtt_username,
        .credentials.client_id = s_device_id,
        .credentials.authentication.password = config->mqtt_password,
        .session.keepalive = 60,
        .session.last_will = {.topic = s_topics[BASE_TOPIC_STATUS], .msg = s_will,
                              .msg_len = (int)strlen(s_will), .qos = 1, .retain = 1},
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 15000,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };
    s_mqtt = esp_mqtt_client_init(&mqtt);
    if (!s_mqtt || esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_event, NULL) != ESP_OK ||
        esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt client unavailable");
        if (s_mqtt) esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
}

/* 应答发出后（或 3 秒后）再执行会断开连接的动作。 */
static void defer(deferred_t action, int message_id)
{
    s_deferred = action;
    s_deferred_message_id = message_id;
    timer_start(TIMER_DEFERRED, DEFERRED_FALLBACK_MS);
}

static void run_deferred(void)
{
    const deferred_t action = s_deferred;
    s_deferred = DEFER_NONE;
    esp_timer_stop(s_timers[TIMER_DEFERRED]);
    if (action == DEFER_RESTART) {
        ESP_LOGI(TAG, "restarting");
        esp_restart();
    } else if (action == DEFER_APPLY_CONFIG) {
        /* 试用新配置：连上 Broker 才保存，超时恢复旧配置。 */
        mqtt_stop();
        if (s_trial_wifi) base_net_reconfigure(s_trial.wifi_ssid, s_trial.wifi_password);
        mqtt_start(&s_trial);
        timer_start(TIMER_CONFIG_TRIAL, CONFIG_TRIAL_MS);
    }
}

static void ota_apply(unsigned actions)
{
    if (actions & BASE_OTA_ACT_PERSIST) {
        const esp_err_t saved = base_store_save_ota(&s_ota.record);
        if (saved != ESP_OK) ESP_LOGE(TAG, "ota record not saved: %s", esp_err_to_name(saved));
    }
    if (actions & BASE_OTA_ACT_CANCEL) base_ota_worker_cancel();
    if (actions & BASE_OTA_ACT_STOP_TIMER) esp_timer_stop(s_timers[TIMER_OTA_RETRY]);
    if (actions & BASE_OTA_ACT_RETRY_TIMER)
        timer_start(TIMER_OTA_RETRY, base_ota_retry_delay_ms(&s_ota, esp_random()));
    int message_id = -1;
    if (actions & BASE_OTA_ACT_PUBLISH) {
        char state[448];
        if (base_ota_state_encode(&s_ota, state, sizeof state) > 0)
            message_id = publish(BASE_TOPIC_OTA_STATE, state, true);
        ESP_LOGI(TAG, "ota job=%s state=%s offset=%" PRIu32 " error=%s", s_ota.record.job,
                 base_ota_phase_name((base_ota_phase_t)s_ota.record.phase), s_ota.written, s_ota.record.error);
    }
    if (actions & BASE_OTA_ACT_REBOOT) defer(DEFER_RESTART, message_id);
    if (actions & BASE_OTA_ACT_START) {
        if (base_ota_worker_start(s_ota.desired.url, s_ota.record.sha256, s_ota.record.size,
                                  s_ota.record.offset, ota_worker_event))
            base_net_set_power_save(false); /* 下载期间关闭省电：省电时每个下行包都要等 DTIM，经高延迟链路吞吐减半 */
        else
            ota_apply(base_ota_on_worker_done(&s_ota, BASE_OTA_RESULT_RETRY, "start"));
    }
}

static void handle_desired(const char *payload, size_t length)
{
    base_ota_desired_t desired;
    const char *error = NULL;
    switch (base_ota_desired_decode(payload, length, &desired, &error)) {
    case BASE_DESIRED_OK: ota_apply(base_ota_on_desired(&s_ota, &desired)); break;
    case BASE_DESIRED_EMPTY: ota_apply(base_ota_on_cancel(&s_ota)); break;
    case BASE_DESIRED_REJECT: ota_apply(base_ota_on_invalid_desired(&s_ota, desired.job, error)); break;
    default: ESP_LOGW(TAG, "unreadable ota/desired ignored"); break;
    }
}

static void handle_command(const char *name, const char *payload, size_t length)
{
    static base_cmd_t command; /* 只在控制任务里用，不占栈 */
    const char *error = NULL;
    const bool decoded = base_cmd_decode(name, payload, length, &command, &error);
    if (!decoded && !command.request_id[0]) return;
    const char *cached = base_dedupe_find(&s_dedupe, command.request_id);
    if (cached) {
        publish(BASE_TOPIC_RSP, cached, false);
        return;
    }
    char result[48];
    const char *result_json = NULL;
    deferred_t after = DEFER_NONE;
    if (decoded) {
        switch (command.kind) {
        case BASE_CMD_STATUS: break;
        case BASE_CMD_RESTART:
            if (s_deferred != DEFER_NONE) error = "busy";
            else after = DEFER_RESTART;
            break;
        case BASE_CMD_CONFIG:
            if (s_deferred != DEFER_NONE || s_trial_active) { error = "busy"; break; }
            s_trial = s_config;
            s_trial_wifi = command.config.has_wifi;
            if (command.config.has_wifi) {
                strcpy(s_trial.wifi_ssid, command.config.wifi_ssid);
                strcpy(s_trial.wifi_password, command.config.wifi_password);
            }
            if (command.config.has_mqtt) {
                strcpy(s_trial.mqtt_uri, command.config.mqtt_uri);
                strcpy(s_trial.mqtt_username, command.config.mqtt_username);
                strcpy(s_trial.mqtt_password, command.config.mqtt_password);
            }
            s_trial_active = true;
            s_config_error = NULL;
            after = DEFER_APPLY_CONFIG;
            break;
        case BASE_CMD_BUSINESS:
            error = base_business_apply(&s_business, command.business_op, command.business_bytes);
            if (base_business_encode(&s_business, result, sizeof result) > 0) result_json = result;
            break;
        }
    }
    char response[BASE_RSP_BYTES];
    if (base_rsp_encode(response, command.request_id, name, error, result_json) < 0) return;
    base_dedupe_store(&s_dedupe, command.request_id, response);
    const int message_id = publish(BASE_TOPIC_RSP, response, false);
    memset(&command, 0, sizeof command); /* 不保留凭据 */
    if (error) return;
    publish_status();
    if (after != DEFER_NONE) defer(after, message_id);
}

static void on_mqtt_connected(void)
{
    s_mqtt_connected = true;
    if (s_trial_active) {
        /* 新配置可用：保存。 */
        s_config = s_trial;
        s_trial_active = false;
        esp_timer_stop(s_timers[TIMER_CONFIG_TRIAL]);
        memset(&s_trial, 0, sizeof s_trial);
        const esp_err_t saved = base_store_save_config(&s_config);
        s_config_error = saved == ESP_OK ? NULL : "not_saved";
    }
    if (s_pending_verify) {
        /* D2：新固件连上 Broker 才确认有效。 */
        const esp_err_t marked = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware confirmed: %s", esp_err_to_name(marked));
        if (marked == ESP_OK) {
            s_pending_verify = false;
            esp_timer_stop(s_timers[TIMER_ROLLBACK]);
        }
    }
    esp_mqtt_client_subscribe(s_mqtt, s_topics[BASE_TOPIC_CMD_FILTER], 1);
    esp_mqtt_client_subscribe(s_mqtt, s_topics[BASE_TOPIC_OTA_DESIRED], 1);
    publish_status();
    if (!s_pending_verify) ota_apply(base_ota_on_confirmed(&s_ota));
}

static void on_timer(timer_id_t id)
{
    switch (id) {
    case TIMER_STATUS: publish_status(); break;
    case TIMER_OTA_RETRY: ota_apply(base_ota_on_retry_timer(&s_ota)); break;
    case TIMER_ROLLBACK:
        if (!s_pending_verify) break;
        ESP_LOGE(TAG, "broker not reached within %u ms; rolling back", (unsigned)ROLLBACK_WINDOW_MS);
        esp_ota_mark_app_invalid_rollback_and_reboot();
        break;
    case TIMER_CONFIG_TRIAL:
        if (!s_trial_active) break;
        ESP_LOGW(TAG, "new configuration did not reach the broker; restoring the previous one");
        s_trial_active = false;
        s_config_error = "trial_failed";
        mqtt_stop();
        if (s_trial_wifi) base_net_reconfigure(s_config.wifi_ssid, s_config.wifi_password);
        memset(&s_trial, 0, sizeof s_trial);
        mqtt_start(&s_config);
        break;
    case TIMER_DEFERRED: run_deferred(); break;
    default: break;
    }
}

static void handle(const event_t *event)
{
    switch ((event_kind_t)event->kind) {
    case EV_NET_UP:
    case EV_NET_TIME:
        /* 严格 TLS 要校验证书有效期：时间同步后才连接 Broker。 */
        if (base_net_time_valid()) mqtt_start(s_trial_active ? &s_trial : &s_config);
        break;
    case EV_NET_DOWN: break;
    case EV_MQTT_CONNECTED: on_mqtt_connected(); break;
    case EV_MQTT_DISCONNECTED: s_mqtt_connected = false; break;
    case EV_MQTT_PUBLISHED:
        if (s_deferred != DEFER_NONE && event->value == s_deferred_message_id) run_deferred();
        break;
    case EV_MQTT_MESSAGE: {
        const char *payload = event->message + event->topic_len + 1;
        char name[BASE_CMD_NAME_BYTES];
        if (!strcmp(event->message, s_topics[BASE_TOPIC_OTA_DESIRED])) handle_desired(payload, event->payload_len);
        else if (base_topic_command(event->message, event->topic_len, s_device_id, name))
            handle_command(name, payload, event->payload_len);
        break;
    }
    case EV_TIMER: on_timer((timer_id_t)event->value); break;
    case EV_OTA_PROGRESS: ota_apply(base_ota_on_progress(&s_ota, (uint32_t)event->value)); break;
    case EV_OTA_VERIFYING: ota_apply(base_ota_on_verifying(&s_ota)); break;
    case EV_OTA_DONE:
        base_net_set_power_save(true);
        ota_apply(base_ota_on_worker_done(&s_ota, (base_ota_result_t)event->value,
                                          event->error[0] ? event->error : NULL));
        break;
    }
}

static void random_uuid(char out[BASE_UUID_BYTES])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    snprintf(out, BASE_UUID_BYTES, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void app_main(void)
{
    s_reset_reason = reset_reason();
    s_events = xQueueCreate(16, sizeof(event_t));
    for (int id = 0; id < TIMER_COUNT; ++id) {
        const esp_timer_create_args_t timer = {.callback = timer_fired, .arg = (void *)(intptr_t)id, .name = "base"};
        ESP_ERROR_CHECK(esp_timer_create(&timer, &s_timers[id]));
    }
    static base_fw_t running;
    uint32_t slot_bytes = 0;
    if (!s_events || !base_ota_running_firmware(&running, &slot_bytes, &s_pending_verify)) {
        ESP_LOGE(TAG, "running firmware unreadable");
        return;
    }
    /* D2 的回滚窗口从最早处开始计时，后面任何一步卡住都会回到旧固件。 */
    if (s_pending_verify) timer_start(TIMER_ROLLBACK, ROLLBACK_WINDOW_MS);

    esp_err_t result = base_store_init();
    if (result == ESP_OK) result = base_store_device_id(s_device_id);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable: %s", esp_err_to_name(result));
    } else {
        random_uuid(s_boot_id);
        for (int topic = 0; topic < 5; ++topic) base_topic(s_topics[topic], s_device_id, (base_topic_t)topic);
        snprintf(s_will, sizeof s_will,
                 "{\"online\":false,\"device_id\":\"%s\",\"boot_id\":\"%s\","
                 "\"fw\":{\"version\":\"%s\",\"sha256\":\"%s\",\"target\":\"%s\",\"slot\":\"%s\"}}",
                 s_device_id, s_boot_id, running.version, running.sha256, running.target, running.slot);
        static base_ota_record_t stored;
        const bool have_record = base_store_load_ota(&stored) == ESP_OK;
        ota_apply(base_ota_boot(&s_ota, have_record ? &stored : NULL, &running, slot_bytes, !s_pending_verify) &
                  BASE_OTA_ACT_PERSIST);
        ESP_LOGI(TAG, "ESP_BASE_BOOT device_id=%s boot_id=%s version=%s target=%s slot=%s sha256=%s reset=%s pending_verify=%d",
                 s_device_id, s_boot_id, running.version, running.target, running.slot, running.sha256,
                 s_reset_reason, (int)s_pending_verify);
        result = base_store_load_config(&s_config);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "ESP_BASE_UNPROVISIONED Wi-Fi/MQTT configuration missing (%s)", esp_err_to_name(result));
        } else {
            result = base_net_start(s_config.wifi_ssid, s_config.wifi_password, net_event);
            if (result != ESP_OK) ESP_LOGE(TAG, "network unavailable: %s", esp_err_to_name(result));
            esp_timer_start_periodic(s_timers[TIMER_STATUS], (uint64_t)STATUS_REFRESH_MS * 1000u);
        }
    }
    /* 没有事件时一直阻塞在这里。 */
    for (event_t event;;) {
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) continue;
        ++s_wakeups;
        handle(&event);
        free(event.message);
    }
}
