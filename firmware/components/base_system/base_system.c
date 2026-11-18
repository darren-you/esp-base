// SPDX-License-Identifier: Apache-2.0
// ESP Base 系统层的主程序：一个控制任务阻塞在事件队列上，Wi-Fi、系统 MQTT、定时器、OTA 下载任务和业务运行时
// 都只向队列投递事件。业务层由业务运行时（base_runtime）启动，这里不接触业务的连接和数据。
#include "base_system.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "base_guard.h"
#include "base_net.h"
#include "base_ota.h"
#include "base_profile.h"
#include "base_protocol.h"
#include "base_runtime_system.h"
#include "base_store.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
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
#define CONFIG_TRIAL_MS 120000u         /* 新 Wi-Fi 须在此时间内连上 Broker，否则恢复旧配置 */
#define BUSINESS_FALLBACK_MS 60000u     /* 系统 MQTT 迟迟连不上时，业务连接最多等这么久 */
#define BUSINESS_REPORT_MS 2000u        /* 业务连接状态变化后，合并这么久再发布状态文档 */
#define DEFERRED_FALLBACK_MS 3000u      /* 等应答发出的最长时间 */
#define STATUS_REFRESH_MS (15u * 60u * 1000u)
#define MESSAGE_MAX_BYTES 1536u
#define STATUS_BYTES 3072 /* 状态文档最长约 2.7 KiB（系统任务、业务连接和业务任务都写满时），与发送缓冲相同 */

typedef enum {
    EV_NET_UP, EV_NET_DOWN, EV_NET_TIME,
    EV_MQTT_CONNECTED, EV_MQTT_DISCONNECTED, EV_MQTT_MESSAGE, EV_MQTT_PUBLISHED,
    EV_TIMER, EV_OTA_PROGRESS, EV_OTA_VERIFYING, EV_OTA_DONE, EV_NET_IP6, EV_PROFILE_READY, EV_BUSINESS_CHANGED,
} event_kind_t;
typedef enum {
    TIMER_STATUS, TIMER_OTA_RETRY, TIMER_ROLLBACK, TIMER_CONFIG_TRIAL, TIMER_DEFERRED,
    TIMER_BUSINESS_CONNECT, TIMER_BUSINESS_REPORT, TIMER_BUSINESS_STABLE, TIMER_COUNT,
} timer_id_t;
typedef enum { DEFER_NONE, DEFER_RESTART, DEFER_APPLY_CONFIG, DEFER_RECONNECT } deferred_t;

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
static char s_will[640];
static const char *s_reset_reason = "other";
static base_config_t s_config, s_trial;
static bool s_trial_active;
static const char *s_config_error;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected, s_pending_verify;
static base_ota_t s_ota;
static base_dedupe_t s_dedupe;
static deferred_t s_deferred;
static int s_deferred_message_id;
static uint32_t s_wakeups;
static base_guard_t s_guard;
/* 上一次运行的固件，留在 RTC 内存里（软件重启、panic 和看门狗复位都不清）：异常复位只记在出事的那个固件头上，
 * 新固件崩溃后回滚回来的旧固件不背这次复位。 */
#define LAST_RUN_MAGIC 0x6c617374u
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    char fw[BASE_SHA256_HEX_BYTES];
} s_last_run;
static bool s_business_connected, s_business_report_pending;

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
    static const uint8_t kinds[] = {EV_NET_UP, EV_NET_DOWN, EV_NET_TIME, EV_NET_IP6};
    post(&(event_t){.kind = kinds[event]}, 0);
}

static void ota_worker_event(base_ota_worker_event_t event, uint32_t value, const char *error)
{
    event_t posted = {.value = (int32_t)value};
    if (event == BASE_OTA_WORKER_PROGRESS) {
        posted.kind = EV_OTA_PROGRESS;
        post(&posted, 0); /* 队列满时丢弃进度，下一个扇区会再报 */
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
    /* 运行时因业务看门狗超时重启设备：复位原因是软件重启，单独标出来。 */
    if (base_runtime_take_fault()) return "business_watchdog";
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

/* 把控制任务持有的设置交给 Profile；状态文档与遗嘱只按 Profile 编码。 */
static void profile_sync(void)
{
    base_profile_set(&(base_profile_settings_t){
        .name = s_config.name, .group = s_config.group, .mqtt_uri = s_config.mqtt_uri,
        .config_revision = s_config.revision, .config_error = s_config_error, .wakeups = s_wakeups,
    });
}

/* 请求异步采样，采完（EV_PROFILE_READY）再发布状态文档。 */
static void publish_status(void)
{
    profile_sync();
    base_profile_request();
}

static void profile_ready(void)
{
    post(&(event_t){.kind = EV_PROFILE_READY}, pdMS_TO_TICKS(100));
}

/* 业务的连接状态变了：在业务的任务里调用，只投递事件。 */
static void business_changed(void)
{
    post(&(event_t){.kind = EV_BUSINESS_CHANGED}, 0);
}

static void publish_profile(void)
{
    char *json = malloc(STATUS_BYTES);
    if (json && base_profile_status(json, STATUS_BYTES) > 0) publish(BASE_TOPIC_STATUS, json, true);
    else ESP_LOGW(TAG, "status document not encoded");
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
    /* 遗嘱带名称和 Group：每次建立连接都按当前设置重写。 */
    profile_sync();
    if (base_profile_will(s_will, sizeof s_will) < 0) s_will[0] = '\0';
#if CONFIG_ESP_BASE_TEST_NO_BROKER
    ESP_LOGW(TAG, "test build: broker connection disabled, rollback expected");
    return;
#endif
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
        .buffer.out_size = 3072, /* 一次发布要放下整个状态文档 */
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
    } else if (action == DEFER_RECONNECT) {
        /* 名称或 Group 变了：断开（正常断开不触发遗嘱）再连，让 Broker 记住新的遗嘱。 */
        mqtt_stop();
        mqtt_start(&s_config);
    } else if (action == DEFER_APPLY_CONFIG) {
        /* 试用新 Wi-Fi：连上 Broker 才保存，超时恢复旧配置。 */
        mqtt_stop();
        base_net_reconfigure(s_trial.wifi_ssid, s_trial.wifi_password);
        mqtt_start(&s_trial);
        timer_start(TIMER_CONFIG_TRIAL, CONFIG_TRIAL_MS);
    }
}

static void ota_apply(unsigned actions)
{
    if (actions & BASE_OTA_ACT_PERSIST) {
        const esp_err_t saved = base_store_save_record(BASE_STORE_OTA, &s_ota.record, sizeof s_ota.record);
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

/* 业务层的连接：时间同步后让业务建立一次，业务 MQTT 配置变更后再让它按新配置重建。 */
static void business_connect(bool changed)
{
    if (!base_runtime_active() || !base_net_time_valid()) return;
    if (s_business_connected && !changed) return;
    s_business_connected = true;
    esp_timer_stop(s_timers[TIMER_BUSINESS_CONNECT]);
    base_runtime_connect();
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
    deferred_t after = DEFER_NONE;
    bool reconnect_business = false;
    if (decoded) {
        switch (command.kind) {
        case BASE_CMD_STATUS: break;
        case BASE_CMD_RESTART:
            if (s_deferred != DEFER_NONE) error = "busy";
            else after = DEFER_RESTART;
            break;
        case BASE_CMD_CONFIG:
            if (s_deferred != DEFER_NONE || s_trial_active) { error = "busy"; break; }
            s_config_error = NULL;
            if (command.config.has_business_mqtt || command.config.has_name || command.config.has_group) {
                /* 业务 MQTT 由业务层自己连接，系统不连接它、无从试用；名称和 Group 只是资料。都收到即保存。 */
                if (command.config.has_business_mqtt) {
                    s_config.business_mqtt = command.config.business_mqtt;
                    base_runtime_set_mqtt(&s_config.business_mqtt);
                }
                if (command.config.has_name) strcpy(s_config.name, command.config.name);
                if (command.config.has_group) strcpy(s_config.group, command.config.group);
                if (base_store_save_config(&s_config) != ESP_OK) s_config_error = "not_saved";
                reconnect_business = command.config.has_business_mqtt;
                /* 遗嘱里也有名称和 Group：应答发出后重建系统 MQTT 连接。同时改 Wi-Fi 时试用本身就会重连。 */
                if (command.config.has_name || command.config.has_group) after = DEFER_RECONNECT;
            }
            if (command.config.has_wifi) {
                s_trial = s_config;
                strcpy(s_trial.wifi_ssid, command.config.wifi_ssid);
                strcpy(s_trial.wifi_password, command.config.wifi_password);
                s_trial_active = true;
                after = DEFER_APPLY_CONFIG;
            }
            break;
        }
    }
    char response[BASE_RSP_BYTES];
    if (base_rsp_encode(response, command.request_id, name, error) < 0) return;
    base_dedupe_store(&s_dedupe, command.request_id, response);
    const int message_id = publish(BASE_TOPIC_RSP, response, false);
    memset(&command, 0, sizeof command); /* 不保留凭据 */
    if (error) return;
    publish_status();
    if (reconnect_business) business_connect(true); /* 应答发出之后再重建业务连接 */
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
    business_connect(false);
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
        ESP_LOGW(TAG, "new Wi-Fi did not reach the broker; restoring the previous one");
        s_trial_active = false;
        s_config_error = "trial_failed";
        mqtt_stop();
        base_net_reconfigure(s_config.wifi_ssid, s_config.wifi_password);
        memset(&s_trial, 0, sizeof s_trial);
        mqtt_start(&s_config);
        break;
    case TIMER_DEFERRED: run_deferred(); break;
    case TIMER_BUSINESS_CONNECT: business_connect(false); break;
    case TIMER_BUSINESS_REPORT:
        s_business_report_pending = false;
        if (s_mqtt_connected) publish_status();
        break;
    case TIMER_BUSINESS_STABLE:
        /* 业务稳定运行了一段时间：之前的异常重启不再计数。 */
        if (base_guard_stable(&s_guard)) base_store_save_record(BASE_STORE_GUARD, &s_guard, sizeof s_guard);
        break;
    default: break;
    }
}

static void handle(const event_t *event)
{
    switch ((event_kind_t)event->kind) {
    case EV_NET_UP:
    case EV_NET_TIME:
        /* 严格 TLS 要校验证书有效期：时间同步后才连接 Broker。 */
        if (base_net_time_valid()) {
            mqtt_start(s_trial_active ? &s_trial : &s_config);
            /* 业务连接等系统 MQTT 先连上再建：两次 TLS 握手同时进行会让单核芯片连续数秒占满 CPU。 */
            if (base_runtime_active() && !s_business_connected) timer_start(TIMER_BUSINESS_CONNECT, BUSINESS_FALLBACK_MS);
        }
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
    case EV_NET_IP6: if (s_mqtt_connected) publish_status(); break; /* SLAAC 地址常在连上 Broker 之后才到 */
    case EV_PROFILE_READY: publish_profile(); break;
    case EV_BUSINESS_CHANGED:
        /* 业务连接抖动时不逐次发布：合并一小段时间。 */
        if (s_business_report_pending) break;
        s_business_report_pending = true;
        timer_start(TIMER_BUSINESS_REPORT, BUSINESS_REPORT_MS);
        break;
    case EV_OTA_PROGRESS: ota_apply(base_ota_on_progress(&s_ota, (uint32_t)event->value)); break;
    case EV_OTA_VERIFYING: ota_apply(base_ota_on_verifying(&s_ota)); break;
    case EV_OTA_DONE:
        base_net_set_power_save(true);
        ota_apply(base_ota_on_worker_done(&s_ota, (base_ota_result_t)event->value,
                                          event->error[0] ? event->error : NULL));
        break;
    }
}

/* 业务守卫：同一个固件连续异常重启到上限就不拉起业务（安全模式），系统照常运行。 */
static void business_boot(const base_business_t *business, const char *fw_sha256, bool abnormal)
{
    bool enabled = true;
    if (business) {
        const bool loaded = base_store_load_record(BASE_STORE_GUARD, &s_guard, sizeof s_guard) == ESP_OK;
        bool save = false;
        enabled = base_guard_boot(&s_guard, loaded, fw_sha256, abnormal, &save);
        if (save) {
            const esp_err_t saved = base_store_save_record(BASE_STORE_GUARD, &s_guard, sizeof s_guard);
            if (saved != ESP_OK) ESP_LOGE(TAG, "business guard not saved: %s", esp_err_to_name(saved));
        }
        if (s_guard.strikes) ESP_LOGW(TAG, "business guard: %u abnormal restart(s) in a row", (unsigned)s_guard.strikes);
    }
    base_runtime_boot(business, s_device_id, &s_config.business_mqtt, enabled, BASE_GUARD_ERROR, business_changed);
    if (base_runtime_active() && s_guard.strikes) timer_start(TIMER_BUSINESS_STABLE, BASE_GUARD_STABLE_MS);
}

void base_system_run(const base_business_t *business)
{
    /* 系统任务的优先级都高于业务任务：业务占满 CPU 时仍能应答命令和升级。 */
    vTaskPrioritySet(NULL, BASE_SYSTEM_TASK_PRIORITY);
    s_reset_reason = reset_reason();
    const esp_reset_reason_t hardware_reset = esp_reset_reason();
    const bool abnormal_reset = !strcmp(s_reset_reason, "business_watchdog") || hardware_reset == ESP_RST_PANIC ||
                                hardware_reset == ESP_RST_INT_WDT || hardware_reset == ESP_RST_TASK_WDT ||
                                hardware_reset == ESP_RST_WDT;
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
    const bool abnormal = abnormal_reset && s_last_run.magic == LAST_RUN_MAGIC && !strcmp(s_last_run.fw, running.sha256);
    s_last_run.magic = LAST_RUN_MAGIC;
    snprintf(s_last_run.fw, sizeof s_last_run.fw, "%s", running.sha256);

    esp_err_t result = base_store_init();
    if (result == ESP_OK) result = base_store_device_id(s_device_id);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable: %s", esp_err_to_name(result));
    } else {
        uint8_t random[16];
        esp_fill_random(random, sizeof random);
        base_uuid_format(s_boot_id, random);
        for (int topic = 0; topic < 5; ++topic) base_topic(s_topics[topic], s_device_id, (base_topic_t)topic);
        const esp_err_t profiled = base_profile_start(s_device_id, s_boot_id, s_reset_reason, &running, profile_ready);
        if (profiled != ESP_OK) ESP_LOGE(TAG, "profile unavailable: %s", esp_err_to_name(profiled));
        static base_ota_record_t stored;
        const bool have_record = base_store_load_record(BASE_STORE_OTA, &stored, sizeof stored) == ESP_OK;
        ota_apply(base_ota_boot(&s_ota, have_record ? &stored : NULL, &running, slot_bytes, !s_pending_verify) &
                  BASE_OTA_ACT_PERSIST);
        ESP_LOGI(TAG, "ESP_BASE_BOOT device_id=%s boot_id=%s version=%s target=%s slot=%s sha256=%s reset=%s pending_verify=%d",
                 s_device_id, s_boot_id, running.version, running.target, running.slot, running.sha256,
                 s_reset_reason, (int)s_pending_verify);
        result = base_store_load_config(&s_config);
        business_boot(business, running.sha256, abnormal);
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
