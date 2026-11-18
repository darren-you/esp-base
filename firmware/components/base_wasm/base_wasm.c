// SPDX-License-Identifier: Apache-2.0
// WASM 宿主在设备上的部分：作为一个业务跑在 base_runtime 上，实现宿主接口背后的平台服务（wasm_port.h），
// 并用一个任务把定时器、业务 MQTT 和系统通知排成事件，逐个交给应用。
//
// 一个应用任务独占应用实例、连接和定时器的状态；定时器回调、MQTT 回调和系统的 connect 回调只向它的队列
// 投递事件，所以没有共享的可变状态要加锁（定时器的两个字段除外，用临界区）。
#include "base_wasm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "base_runtime.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wasm_host.h"
#include "wasm_port.h"

#define TASK_STACK_BYTES 5120 /* 实测最多用到约 2 KB（加载样例应用时） */
#define TASK_PRIORITY 3
#define QUEUE_LENGTH 16
#define PENDING_BYTES_MAX 4096 /* 排队等应用处理的消息数据上限；超出的消息丢弃并计数 */
#define SUBSCRIPTIONS 4
#define RECONNECT_MS 10000 /* 与系统 MQTT 的 5 秒错开，避免两条连接同时做 TLS 握手 */
#define RESTART_MIN_MS 5000
#define RESTART_MAX_MS 300000
#define STABLE_MS 300000 /* 应用连续运行这么久算稳定：之后再出故障，重启间隔从头算 */
#define UNIX_MS_2020 1577836800000ull

/* 可行性阶段的固定限额；应用清单（T4）落地后改由清单给出。 */
#define POOL_BYTES (8 * 1024)
#define STACK_BYTES 2048
#define LINEAR_MAX_BYTES (16 * 1024)
#define BUDGET 2000000

typedef enum { EV_TIMER, EV_LINK_UP, EV_LINK_DOWN, EV_LINK_MESSAGE, EV_SYSTEM_CONNECT, EV_RESTART } kind_t;

typedef struct {
    uint8_t kind;
    uint8_t timer;
    uint16_t topic_length, payload_length;
    uint32_t epoch;                  /* EV_TIMER：定时器设置时的代，对不上说明它后来被重设或取消了 */
    esp_mqtt_client_handle_t client; /* 产生事件的连接；与当前连接不同的事件来自已关闭的连接，丢弃 */
    char *data;                      /* EV_LINK_MESSAGE：主题紧跟载荷，处理后释放 */
} event_t;

typedef struct {
    esp_timer_handle_t handle;
    uint32_t period_ms, epoch;
    int64_t due_us; /* 这一次设置最早的到期时刻：早于它的回调属于上一次设置 */
    bool periodic;  /* 首次到期后已经转为周期运行 */
} app_timer_t;

static const char *TAG = "base_wasm";
static QueueHandle_t s_queue;
static portMUX_TYPE s_timer_lock = portMUX_INITIALIZER_UNLOCKED;
static app_timer_t s_timers[WASM_TIMERS];
static esp_timer_handle_t s_restart;
static uint32_t s_restart_ms = RESTART_MIN_MS;
static int64_t s_started_us;
static atomic_uint s_pending_bytes, s_dropped;
static uint8_t *s_image;

/* 业务连接：只在应用任务里访问。 */
static struct {
    bool wanted;  /* 应用要求过连接 */
    bool allowed; /* 系统已经通知可以建立业务连接（系统 MQTT 先连上，两次握手错开） */
    bool connected;
    esp_mqtt_client_handle_t client;
    char *will_topic;
    uint8_t *will_payload;
    size_t will_payload_length;
    int will_qos;
    bool will_retain;
    unsigned keepalive_s;
    struct {
        char *filter;
        int qos;
    } subscriptions[SUBSCRIPTIONS];
    char client_id[48];
} s_link;

extern const uint8_t hello_start[] asm("_binary_hello_wasm_start");
extern const uint8_t hello_end[] asm("_binary_hello_wasm_end");

static void post(const event_t *event)
{
    if (xQueueSend(s_queue, event, 0) == pdTRUE) return;
    if (event->data) {
        atomic_fetch_sub(&s_pending_bytes, (unsigned)(event->topic_length + event->payload_length));
        free(event->data);
    }
    atomic_fetch_add(&s_dropped, 1);
}

/* WAMR 取当前线程标识的入口，链接时取代它的 ESP-IDF 端口里基于 pthread_self() 的实现（见 CMakeLists.txt）。 */
unsigned int __wrap_os_self_thread(void)
{
    return (unsigned int)(uintptr_t)xTaskGetCurrentTaskHandle();
}

/* ---------- 平台服务：时间与定时器 ---------- */

void wasm_port_log(int level, const char *text, size_t length)
{
    const esp_log_level_t mapped = level <= 1 ? ESP_LOG_ERROR : level == 2 ? ESP_LOG_WARN : ESP_LOG_INFO;
    ESP_LOG_LEVEL(mapped, "app", "%.*s", (int)(length > 200 ? 200 : length), text);
}

uint64_t wasm_port_uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint64_t wasm_port_unix_ms(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    const uint64_t ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u;
    return ms >= UNIX_MS_2020 ? ms : 0;
}

void wasm_port_sleep_ms(uint32_t ms)
{
    if (ms) vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

static void timer_fired(void *argument)
{
    app_timer_t *timer = argument;
    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_timer_lock);
    const uint32_t epoch = timer->epoch, period_ms = timer->period_ms;
    const bool stale = now_us + 500 < timer->due_us; /* 应用在回调执行前重设了这个定时器 */
    const bool start_periodic = !stale && period_ms && !timer->periodic;
    if (start_periodic) timer->periodic = true;
    taskEXIT_CRITICAL(&s_timer_lock);
    if (stale) return;
    if (start_periodic) esp_timer_start_periodic(timer->handle, (uint64_t)period_ms * 1000u);
    post(&(event_t){.kind = EV_TIMER, .timer = (uint8_t)(timer - s_timers), .epoch = epoch});
}

int wasm_port_timer_cancel(unsigned id)
{
    app_timer_t *timer = &s_timers[id];
    esp_timer_stop(timer->handle);
    taskENTER_CRITICAL(&s_timer_lock);
    ++timer->epoch; /* 已经排在队列里的到期事件作废 */
    timer->period_ms = 0;
    timer->periodic = false;
    taskEXIT_CRITICAL(&s_timer_lock);
    return WASM_OK;
}

int wasm_port_timer_set(unsigned id, uint32_t delay_ms, uint32_t period_ms)
{
    app_timer_t *timer = &s_timers[id];
    wasm_port_timer_cancel(id);
    taskENTER_CRITICAL(&s_timer_lock);
    timer->period_ms = period_ms;
    timer->due_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
    taskEXIT_CRITICAL(&s_timer_lock);
    return esp_timer_start_once(timer->handle, (uint64_t)delay_ms * 1000u) == ESP_OK ? WASM_OK : WASM_E_NO_MEMORY;
}

/* ---------- 平台服务：业务 MQTT ---------- */

static void link_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    (void)base;
    const esp_mqtt_event_handle_t event = data;
    event_t posted = {.client = event->client};
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: posted.kind = EV_LINK_UP; break;
    case MQTT_EVENT_DISCONNECTED: posted.kind = EV_LINK_DOWN; break;
    case MQTT_EVENT_DATA: {
        /* 只收一次到齐的小消息：分片的、超长的丢弃并计数。 */
        if (event->current_data_offset != 0 || event->data_len != event->total_data_len || event->topic_len <= 0 ||
            event->topic_len > WASM_TOPIC_BYTES || event->data_len > WASM_PAYLOAD_BYTES) {
            atomic_fetch_add(&s_dropped, 1);
            return;
        }
        /* 排队等应用处理的数据有上限：应用处理不过来时丢新消息，不让队列吃掉系统的内存。 */
        const unsigned bytes = (unsigned)event->topic_len + (unsigned)event->data_len;
        if (atomic_fetch_add(&s_pending_bytes, bytes) + bytes > PENDING_BYTES_MAX || !(posted.data = malloc(bytes))) {
            atomic_fetch_sub(&s_pending_bytes, bytes);
            atomic_fetch_add(&s_dropped, 1);
            return;
        }
        memcpy(posted.data, event->topic, (size_t)event->topic_len);
        memcpy(posted.data + event->topic_len, event->data, (size_t)event->data_len);
        posted.kind = EV_LINK_MESSAGE;
        posted.topic_length = (uint16_t)event->topic_len;
        posted.payload_length = (uint16_t)event->data_len;
        break;
    }
    default: return;
    }
    post(&posted);
}

static void link_close(void)
{
    if (!s_link.client) return;
    /* 主动断开不会触发遗嘱：先替应用把它发出去，订阅方不会看到过期的在线状态。 */
    if (s_link.connected && s_link.will_topic)
        esp_mqtt_client_publish(s_link.client, s_link.will_topic, (const char *)s_link.will_payload,
                                (int)s_link.will_payload_length, s_link.will_qos, s_link.will_retain);
    base_runtime_mqtt_close(s_link.client);
    s_link.client = NULL;
    s_link.connected = false;
}

static void link_open(void)
{
    link_close();
    static base_runtime_mqtt_t config; /* 含密码：用完清零 */
    if (!base_runtime_mqtt_config(&config)) {
        ESP_LOGW(TAG, "ESP_BASE_APP_LINK_IDLE business mqtt is not configured");
        return;
    }
    const esp_mqtt_client_config_t mqtt = {
        .broker.address.uri = config.uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = config.username,
        .credentials.client_id = s_link.client_id,
        .credentials.authentication.password = config.password,
        .session.keepalive = (int)s_link.keepalive_s,
        .session.last_will = {.topic = s_link.will_topic, .msg = (const char *)s_link.will_payload,
                              .msg_len = (int)s_link.will_payload_length, .qos = s_link.will_qos,
                              .retain = s_link.will_retain},
        .network.reconnect_timeout_ms = RECONNECT_MS,
        .network.timeout_ms = 15000,
        .buffer.size = WASM_PAYLOAD_BYTES + WASM_TOPIC_BYTES + 128,
        .buffer.out_size = 1024,
    };
    s_link.client = base_runtime_mqtt_open("mqtt", &mqtt);
    memset(&config, 0, sizeof config);
    if (!s_link.client) return;
    if (esp_mqtt_client_register_event(s_link.client, MQTT_EVENT_ANY, link_event, NULL) != ESP_OK ||
        esp_mqtt_client_start(s_link.client) != ESP_OK) {
        ESP_LOGE(TAG, "business mqtt client unavailable");
        base_runtime_mqtt_close(s_link.client);
        s_link.client = NULL;
    }
}

static void link_forget(void)
{
    link_close();
    free(s_link.will_topic);
    free(s_link.will_payload);
    for (int index = 0; index < SUBSCRIPTIONS; ++index) free(s_link.subscriptions[index].filter);
    const bool allowed = s_link.allowed;
    char client_id[sizeof s_link.client_id];
    memcpy(client_id, s_link.client_id, sizeof client_id);
    memset(&s_link, 0, sizeof s_link);
    s_link.allowed = allowed;
    memcpy(s_link.client_id, client_id, sizeof client_id);
}

static char *copy_string(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

int wasm_port_mqtt_connect(const wasm_port_will_t *will, unsigned keepalive_s)
{
    char *topic = NULL;
    uint8_t *payload = NULL;
    if (will->topic) {
        topic = copy_string(will->topic, will->topic_length);
        payload = malloc(will->payload_length ? will->payload_length : 1);
        if (!topic || !payload) {
            free(topic);
            free(payload);
            return WASM_E_NO_MEMORY;
        }
        if (will->payload_length) memcpy(payload, will->payload, will->payload_length);
    }
    link_close();
    free(s_link.will_topic);
    free(s_link.will_payload);
    s_link.will_topic = topic;
    s_link.will_payload = payload;
    s_link.will_payload_length = will->topic ? will->payload_length : 0;
    s_link.will_qos = will->qos;
    s_link.will_retain = will->retain;
    s_link.keepalive_s = keepalive_s;
    s_link.wanted = true;
    if (s_link.allowed) link_open();
    return WASM_OK;
}

int wasm_port_mqtt_disconnect(void)
{
    s_link.wanted = false;
    link_close();
    return WASM_OK;
}

int wasm_port_mqtt_subscribe(const char *filter, size_t length, int qos)
{
    for (int index = 0; index < SUBSCRIPTIONS; ++index) {
        const char *known = s_link.subscriptions[index].filter;
        if (known && strlen(known) == length && !memcmp(known, filter, length)) return WASM_OK;
    }
    for (int index = 0; index < SUBSCRIPTIONS; ++index) {
        if (s_link.subscriptions[index].filter) continue;
        char *copy = copy_string(filter, length);
        if (!copy) return WASM_E_NO_MEMORY;
        s_link.subscriptions[index].filter = copy;
        s_link.subscriptions[index].qos = qos;
        if (s_link.connected) esp_mqtt_client_subscribe(s_link.client, copy, qos);
        return WASM_OK;
    }
    return WASM_E_NO_MEMORY; /* 订阅槽用完了 */
}

int wasm_port_mqtt_publish(const char *topic, size_t topic_length, const void *payload, size_t payload_length, int qos,
                           bool retain)
{
    if (!s_link.connected) return WASM_E_NOT_READY;
    char name[WASM_TOPIC_BYTES + 1];
    memcpy(name, topic, topic_length);
    name[topic_length] = '\0';
    /* 入队即返回，不在应用任务里等网络。 */
    return esp_mqtt_client_enqueue(s_link.client, name, payload, (int)payload_length, qos, retain, true) >= 0
               ? WASM_OK
               : WASM_E_NO_MEMORY;
}

void wasm_port_release(void)
{
    for (unsigned id = 0; id < WASM_TIMERS; ++id) wasm_port_timer_cancel(id);
    link_forget();
}

/* ---------- 应用任务 ---------- */

static void restart_fired(void *argument)
{
    (void)argument;
    post(&(event_t){.kind = EV_RESTART});
}

static void restart_later(void)
{
    if (s_started_us && esp_timer_get_time() - s_started_us >= (int64_t)STABLE_MS * 1000) s_restart_ms = RESTART_MIN_MS;
    s_started_us = 0;
    ESP_LOGW(TAG, "ESP_BASE_APP_RESTART in %u ms", (unsigned)s_restart_ms);
    esp_timer_stop(s_restart);
    esp_timer_start_once(s_restart, (uint64_t)s_restart_ms * 1000u);
    s_restart_ms = s_restart_ms * 2 > RESTART_MAX_MS ? RESTART_MAX_MS : s_restart_ms * 2;
}

static void start_app(void)
{
    char error[128] = "";
    const uint32_t bytes = (uint32_t)(hello_end - hello_start);
    /* 经典解释器的加载器会原地改写字节码：镜像放在可写的内存里，应用停止前一直保留。 */
    free(s_image);
    if ((s_image = malloc(bytes))) memcpy(s_image, hello_start, bytes);
    const wasm_host_limits_t limits = {.pool_bytes = POOL_BYTES, .stack_bytes = STACK_BYTES,
                                       .linear_max_bytes = LINEAR_MAX_BYTES, .budget = BUDGET};
    if (!s_image || !wasm_host_start(s_image, bytes, &limits, error, sizeof error)) {
        ESP_LOGE(TAG, "ESP_BASE_APP_START_FAILED %s", s_image ? error : "no memory for the image");
        free(s_image);
        s_image = NULL;
        restart_later();
        return;
    }
    s_started_us = esp_timer_get_time();
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    ESP_LOGI(TAG, "ESP_BASE_APP_STARTED image=%u pool=%u/%u peak=%u linear=%u", (unsigned)bytes,
             (unsigned)stats.pool_used, (unsigned)POOL_BYTES, (unsigned)stats.pool_peak, (unsigned)stats.linear_bytes);
}

static void dispatch(wasm_event_kind_t kind, uint32_t a, uint32_t b)
{
    if (!wasm_host_running() || wasm_host_event(kind, a, b)) return;
    ESP_LOGE(TAG, "ESP_BASE_APP_FAULT %s", wasm_host_fault());
    wasm_host_stop();
    free(s_image);
    s_image = NULL;
    restart_later();
}

static void handle(const event_t *event)
{
    switch ((kind_t)event->kind) {
    case EV_SYSTEM_CONNECT:
        s_link.allowed = true;
        if (s_link.wanted) link_open();
        break;
    case EV_LINK_UP:
        if (event->client != s_link.client) break;
        s_link.connected = true;
        for (int index = 0; index < SUBSCRIPTIONS; ++index)
            if (s_link.subscriptions[index].filter)
                esp_mqtt_client_subscribe(s_link.client, s_link.subscriptions[index].filter,
                                          s_link.subscriptions[index].qos);
        dispatch(WASM_EVENT_MQTT_UP, 0, 0);
        break;
    case EV_LINK_DOWN:
        if (event->client != s_link.client) break;
        s_link.connected = false;
        dispatch(WASM_EVENT_MQTT_DOWN, 0, 0);
        break;
    case EV_LINK_MESSAGE:
        if (event->client == s_link.client) {
            wasm_host_event_data(event->data, event->topic_length, event->data + event->topic_length,
                                 event->payload_length);
            dispatch(WASM_EVENT_MQTT_MESSAGE, event->topic_length, event->payload_length);
        }
        atomic_fetch_sub(&s_pending_bytes, (unsigned)(event->topic_length + event->payload_length));
        free(event->data);
        break;
    case EV_TIMER: {
        taskENTER_CRITICAL(&s_timer_lock);
        const bool current = event->epoch == s_timers[event->timer].epoch;
        taskEXIT_CRITICAL(&s_timer_lock);
        if (current) dispatch(WASM_EVENT_TIMER, event->timer, 0);
        break;
    }
    case EV_RESTART:
        if (!wasm_host_running()) start_app();
        break;
    }
}

static void app_task(void *argument)
{
    (void)argument;
    start_app();
    for (event_t event;;) {
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) == pdTRUE) handle(&event);
    }
}

/* 事件队列和定时器：应用启动之前建好，之后一直留着。 */
static bool platform_init(void)
{
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(event_t));
    bool ready = s_queue != NULL;
    for (unsigned id = 0; ready && id < WASM_TIMERS; ++id) {
        const esp_timer_create_args_t timer = {.callback = timer_fired, .arg = &s_timers[id], .name = "app_timer"};
        ready = esp_timer_create(&timer, &s_timers[id].handle) == ESP_OK;
    }
    const esp_timer_create_args_t restart = {.callback = restart_fired, .name = "app_restart"};
    return ready && esp_timer_create(&restart, &s_restart) == ESP_OK;
}

static void business_init(void)
{
    /* 业务连接的客户端 ID 不能用设备 UUID：系统 MQTT 已经在用，同一 Broker 上会互相挤下线。 */
    snprintf(s_link.client_id, sizeof s_link.client_id, "app-%s", base_runtime_device_id());
    if (!platform_init() || base_runtime_task("wasm_app", app_task, NULL, TASK_STACK_BYTES, TASK_PRIORITY, 0) != ESP_OK)
        ESP_LOGE(TAG, "ESP_BASE_APP_UNAVAILABLE resources for the WASM host could not be created");
}

static void business_connect(void)
{
    if (s_queue) post(&(event_t){.kind = EV_SYSTEM_CONNECT});
}

const base_business_t base_wasm_business = {.name = "wasm", .init = business_init, .connect = business_connect};

/* ---------- 可行性实测（darren-space#113 T3） ---------- */
#if CONFIG_ESP_BASE_TEST_WASM_BENCH
#include "esp_heap_caps.h"

#define BENCH_CALLS 1000
#define BENCH_ROUNDS 3

static void bench_report(const char *phase)
{
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    printf("WASM_BENCH phase=%s heap_free=%u heap_min=%u heap_largest=%u pool_used=%u pool_peak=%u linear=%u "
           "stack_min_free=%u\n",
           phase, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), (unsigned)stats.pool_used,
           (unsigned)stats.pool_peak, (unsigned)stats.linear_bytes, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void bench_task(void *argument)
{
    (void)argument;
    static const char topic[] = "hello/in", payload[] = "{\"ping\":1}";
    const uint32_t bytes = (uint32_t)(hello_end - hello_start);
#if CONFIG_WAMR_INTERP_FAST
    const char *interp = "fast";
#else
    const char *interp = "classic";
#endif
    printf("WASM_BENCH begin target=%s interp=%s image_bytes=%u pool_bytes=%u wasm_stack=%u task_stack=%u\n",
           CONFIG_IDF_TARGET, interp, (unsigned)bytes, (unsigned)POOL_BYTES, (unsigned)STACK_BYTES,
           (unsigned)TASK_STACK_BYTES);
    bool ok = platform_init();
    bench_report("idle");
    for (int round = 0; ok && round < BENCH_ROUNDS; ++round) {
        start_app();
        ok = wasm_host_running();
        bench_report("started");
        const int64_t begin = esp_timer_get_time();
        ok = ok && wasm_host_event(WASM_EVENT_MQTT_UP, 0, 0);
        for (int index = 0; ok && index < BENCH_CALLS; ++index) ok = wasm_host_event(WASM_EVENT_TIMER, 0, 0);
        const int64_t elapsed = esp_timer_get_time() - begin;
        wasm_host_event_data(topic, sizeof topic - 1, payload, sizeof payload - 1);
        ok = ok && wasm_host_event(WASM_EVENT_MQTT_MESSAGE, sizeof topic - 1, sizeof payload - 1);
        wasm_host_stats_t stats;
        wasm_host_stats(&stats);
        printf("WASM_BENCH round=%d ok=%d calls=%u faults=%u per_call_us=%d\n", round, ok, (unsigned)stats.calls,
               (unsigned)stats.faults, (int)(elapsed / BENCH_CALLS));
        bench_report("events");
        wasm_host_stop();
        free(s_image);
        s_image = NULL;
        bench_report("stopped"); /* 与 idle 相同才说明没有泄漏 */
    }
    printf("WASM_BENCH done ok=%d dropped=%u\n", ok, (unsigned)atomic_load(&s_dropped));
    vTaskDelete(NULL);
}

void base_wasm_bench(void)
{
    xTaskCreate(bench_task, "wasm_app", TASK_STACK_BYTES, NULL, TASK_PRIORITY, NULL);
    for (;;) vTaskDelay(portMAX_DELAY);
}
#endif
