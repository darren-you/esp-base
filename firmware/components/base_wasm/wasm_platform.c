// SPDX-License-Identifier: Apache-2.0
// 宿主接口背后的平台服务在设备上的实现（core/wasm_port.h）：日志、时间、业务参数、引脚、定时器和业务 MQTT。
//
// 定时器回调和业务连接的回调只向应用任务的队列投递事件；连接的状态只在应用任务里访问，所以不需要锁
// （定时器的几个字段除外，用临界区）。
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "base_runtime.h"
#include "base_wasm_internal.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wasm_port.h"

#define PENDING_BYTES_MAX 4096 /* 排队等应用处理的消息数据上限；超出的消息丢弃并计数 */
#define SUBSCRIPTIONS 4
#define RECONNECT_MS 10000 /* 与系统 MQTT 的 5 秒错开，避免两条连接同时做 TLS 握手 */
#define UNIX_MS_2020 1577836800000ull
#define PWM_TIMERS 4
#define PWM_NONE 0xFFu

typedef struct {
    esp_timer_handle_t handle;
    uint32_t period_ms, epoch;
    int64_t due_us; /* 这一次设置最早的到期时刻：早于它的回调属于上一次设置 */
    bool periodic;  /* 首次到期后已经转为周期运行 */
} app_timer_t;

static const char *TAG = "base_wasm";
static portMUX_TYPE s_timer_lock = portMUX_INITIALIZER_UNLOCKED;
static app_timer_t s_timers[WASM_TIMERS];
static atomic_uint s_pending_bytes, s_dropped;

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

/* 应用声明的引脚，以及 PWM 用掉的 LEDC 定时器：只在应用任务里访问（开机时由建它的任务设置）。 */
static wasm_pin_t s_pins[WASM_PACKAGE_PINS];
static unsigned s_pin_count;
static uint8_t s_pwm_timer[WASM_PACKAGE_PINS]; /* 与 s_pins 对应：这个引脚正在用的 LEDC 定时器；PWM_NONE：没在输出 PWM */
static struct {
    uint32_t frequency_hz;
    uint8_t resolution_bits; /* 0：空闲 */
} s_pwm_timers[PWM_TIMERS];

/* WAMR 取当前线程标识的入口，链接时取代它的 ESP-IDF 端口里基于 pthread_self() 的实现（见 CMakeLists.txt）。 */
unsigned int __wrap_os_self_thread(void)
{
    return (unsigned int)(uintptr_t)xTaskGetCurrentTaskHandle();
}

void wasm_platform_count_drop(void)
{
    atomic_fetch_add(&s_dropped, 1);
}

unsigned wasm_platform_dropped(void)
{
    return atomic_load(&s_dropped);
}

/* ---------- 日志与时间 ---------- */

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

/* ---------- 业务参数与设备信息 ---------- */

int wasm_port_param(const char *key, char *out, size_t capacity)
{
    static char value[256];
    const esp_err_t result = base_runtime_param(key, value, sizeof value);
    if (result == ESP_ERR_NVS_NOT_FOUND) return WASM_E_NOT_FOUND;
    if (result != ESP_OK) return WASM_E_TOO_LARGE;
    const size_t length = strlen(value);
    if (length > capacity) return WASM_E_TOO_LARGE;
    memcpy(out, value, length);
    return (int)length;
}

int wasm_port_info(wasm_info_t what, char *out, size_t capacity)
{
    const char *text = what == WASM_INFO_DEVICE_ID ? base_runtime_device_id() : CONFIG_IDF_TARGET;
    const size_t length = strlen(text);
    if (length > capacity) return WASM_E_TOO_LARGE;
    memcpy(out, text, length);
    return (int)length;
}

/* ---------- 引脚 ----------
 * 引脚归应用所有，这里用不登记占用的基本操作（gpio_set_direction、gpio_set_pull_mode）：同一个引脚先作普通输出、
 * 再交给 LEDC，或者反过来，驱动都不会报冲突。 */

static int pin_slot(unsigned pin)
{
    for (unsigned index = 0; index < s_pin_count; ++index)
        if (s_pins[index].pin == pin) return (int)index;
    return -1;
}

/* PWM 引脚按在清单里的先后占用 LEDC 通道；芯片的通道够不够用在安装时已经核对过。 */
static ledc_channel_t pwm_channel(unsigned index)
{
    unsigned channel = 0;
    for (unsigned before = 0; before < index; ++before) channel += s_pins[before].mode == WASM_PIN_PWM;
    return (ledc_channel_t)channel;
}

/* 没有引脚在用的 LEDC 定时器让出来。 */
static void pwm_timer_release(uint8_t timer)
{
    for (unsigned index = 0; index < s_pin_count; ++index)
        if (s_pwm_timer[index] == timer) return;
    const ledc_timer_config_t release = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = (ledc_timer_t)timer, .deconfigure = true};
    ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)timer); /* 驱动要求先停再撤 */
    ledc_timer_config(&release);
    s_pwm_timers[timer].resolution_bits = 0;
}

/* 停掉一个引脚上的 PWM，把引脚从 LEDC 收回来。驱动收回时会关掉输出，这期间由内部下拉把引脚按在低电平；
 * 调用方接着把它置到安全状态。 */
static void pwm_stop(unsigned index)
{
    const uint8_t timer = s_pwm_timer[index];
    if (timer == PWM_NONE) return;
    const ledc_channel_t channel = pwm_channel(index);
    const ledc_channel_config_t release = {.speed_mode = LEDC_LOW_SPEED_MODE, .channel = channel, .deconfigure = true};
    ledc_stop(LEDC_LOW_SPEED_MODE, channel, 0); /* 输出停在低电平 */
    gpio_pulldown_en((gpio_num_t)s_pins[index].pin);
    ledc_channel_config(&release);
    s_pwm_timer[index] = PWM_NONE;
    pwm_timer_release(timer);
}

/* 把一个引脚置到清单声明的安全状态。 */
static void pin_safe(unsigned index)
{
    const gpio_num_t pin = (gpio_num_t)s_pins[index].pin;
    const wasm_pin_mode_t mode = (wasm_pin_mode_t)s_pins[index].mode;
    pwm_stop(index);
    if (mode == WASM_PIN_OUT0 || mode == WASM_PIN_OUT1 || mode == WASM_PIN_PWM) {
        /* 先写电平再打开输出：引脚不会在切换的瞬间跳到相反的电平。输入也开着，读回的是引脚的实际电平。 */
        gpio_set_level(pin, mode == WASM_PIN_OUT1);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        return;
    }
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, mode == WASM_PIN_IN_PULLUP     ? GPIO_PULLUP_ONLY
                            : mode == WASM_PIN_IN_PULLDOWN ? GPIO_PULLDOWN_ONLY
                                                           : GPIO_FLOATING);
}

void wasm_platform_pins(const wasm_pin_t *pins, unsigned count)
{
    if (count > WASM_PACKAGE_PINS) count = WASM_PACKAGE_PINS;
    /* 先收回所有 PWM（通道是按清单里的先后分的，换了清单要重新分）；这一组里不再声明的引脚还给系统：高阻，不带上下拉。 */
    for (unsigned index = 0; index < s_pin_count; ++index) {
        pwm_stop(index);
        if (wasm_pin_mode(pins, count, s_pins[index].pin) >= 0) continue;
        gpio_set_direction((gpio_num_t)s_pins[index].pin, GPIO_MODE_DISABLE);
        gpio_set_pull_mode((gpio_num_t)s_pins[index].pin, GPIO_FLOATING);
    }
    s_pin_count = count;
    if (count) memcpy(s_pins, pins, count * sizeof s_pins[0]);
    memset(s_pwm_timer, PWM_NONE, sizeof s_pwm_timer);
    for (unsigned index = 0; index < s_pin_count; ++index) pin_safe(index);
}

int wasm_port_gpio_write(unsigned pin, int level)
{
    return pin_slot(pin) >= 0 && gpio_set_level((gpio_num_t)pin, level != 0) == ESP_OK ? WASM_OK : WASM_E_INVALID;
}

int wasm_port_gpio_read(unsigned pin)
{
    return pin_slot(pin) >= 0 ? gpio_get_level((gpio_num_t)pin) : WASM_E_INVALID;
}

int wasm_port_pwm_write(unsigned pin, uint32_t duty)
{
    const int index = pin_slot(pin);
    if (index < 0 || s_pwm_timer[index] == PWM_NONE) return WASM_E_NOT_READY;
    const ledc_channel_t channel = pwm_channel((unsigned)index);
    return ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty) == ESP_OK && ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) == ESP_OK
               ? WASM_OK
               : WASM_E_INVALID;
}

int wasm_port_pwm_read(unsigned pin)
{
    const int index = pin_slot(pin);
    if (index < 0 || s_pwm_timer[index] == PWM_NONE) return WASM_E_NOT_READY;
    return (int)ledc_get_duty(LEDC_LOW_SPEED_MODE, pwm_channel((unsigned)index));
}

int wasm_port_pwm_config(unsigned pin, uint32_t frequency_hz, unsigned resolution_bits)
{
    const int index = pin_slot(pin);
    if (index < 0) return WASM_E_INVALID;
    uint8_t timer = s_pwm_timer[index];
    /* 已经按同样的参数在输出：只把占空比归零。 */
    if (timer != PWM_NONE && s_pwm_timers[timer].frequency_hz == frequency_hz &&
        s_pwm_timers[timer].resolution_bits == resolution_bits)
        return wasm_port_pwm_write(pin, 0);
    pin_safe((unsigned)index); /* 换参数：先停下，回到低电平 */
    /* 频率和分辨率相同的引脚共用一个 LEDC 定时器。 */
    timer = PWM_NONE;
    for (uint8_t at = 0; at < PWM_TIMERS && timer == PWM_NONE; ++at)
        if (s_pwm_timers[at].resolution_bits == resolution_bits && s_pwm_timers[at].frequency_hz == frequency_hz) timer = at;
    for (uint8_t at = 0; at < PWM_TIMERS && timer == PWM_NONE; ++at) {
        if (s_pwm_timers[at].resolution_bits) continue;
        const ledc_timer_config_t config = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = (ledc_timer_bit_t)resolution_bits,
                                            .timer_num = (ledc_timer_t)at, .freq_hz = frequency_hz, .clk_cfg = LEDC_AUTO_CLK};
        if (ledc_timer_config(&config) != ESP_OK) return WASM_E_INVALID; /* 这个频率配不出这样的分辨率 */
        s_pwm_timers[at].frequency_hz = frequency_hz;
        s_pwm_timers[at].resolution_bits = (uint8_t)resolution_bits;
        timer = at;
    }
    if (timer == PWM_NONE) return WASM_E_NO_MEMORY; /* 四个定时器都被别的频率占着 */
    const ledc_channel_config_t channel = {.gpio_num = (int)pin, .speed_mode = LEDC_LOW_SPEED_MODE,
                                           .channel = pwm_channel((unsigned)index), .timer_sel = (ledc_timer_t)timer, .duty = 0};
    if (ledc_channel_config(&channel) != ESP_OK) {
        pwm_timer_release(timer);
        pin_safe((unsigned)index);
        return WASM_E_INVALID;
    }
    s_pwm_timer[index] = timer;
    return WASM_OK;
}

/* ---------- 定时器 ---------- */

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
    base_wasm_post(&(base_wasm_event_t){.kind = EV_TIMER, .timer = (uint8_t)(timer - s_timers), .epoch = epoch});
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

bool wasm_platform_timer_current(uint8_t id, uint32_t epoch)
{
    taskENTER_CRITICAL(&s_timer_lock);
    const bool current = id < WASM_TIMERS && epoch == s_timers[id].epoch;
    taskEXIT_CRITICAL(&s_timer_lock);
    return current;
}

/* ---------- 业务 MQTT ---------- */

static void link_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    (void)base;
    const esp_mqtt_event_handle_t event = data;
    base_wasm_event_t posted = {.client = event->client};
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: posted.kind = EV_LINK_UP; break;
    case MQTT_EVENT_DISCONNECTED: posted.kind = EV_LINK_DOWN; break;
    case MQTT_EVENT_DATA: {
        /* 只收一次到齐的小消息：分片的、超长的丢弃并计数。 */
        if (event->current_data_offset != 0 || event->data_len != event->total_data_len || event->topic_len <= 0 ||
            event->topic_len > WASM_TOPIC_BYTES || event->data_len > WASM_PAYLOAD_BYTES) {
            wasm_platform_count_drop();
            return;
        }
        /* 排队等应用处理的数据有上限：应用处理不过来时丢新消息，不让队列吃掉系统的内存。 */
        const unsigned bytes = (unsigned)event->topic_len + (unsigned)event->data_len;
        if (atomic_fetch_add(&s_pending_bytes, bytes) + bytes > PENDING_BYTES_MAX || !(posted.data = malloc(bytes))) {
            atomic_fetch_sub(&s_pending_bytes, bytes);
            wasm_platform_count_drop();
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
    base_wasm_post(&posted);
}

void wasm_platform_message_done(const base_wasm_event_t *event)
{
    atomic_fetch_sub(&s_pending_bytes, (unsigned)(event->topic_length + event->payload_length));
    free(event->data);
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

void wasm_platform_connect_allowed(void)
{
    s_link.allowed = true;
    if (s_link.wanted) link_open();
}

bool wasm_platform_link_current(void *client)
{
    return s_link.client && client == s_link.client;
}

bool wasm_platform_link_up(void *client)
{
    if (!wasm_platform_link_current(client)) return false;
    s_link.connected = true;
    for (int index = 0; index < SUBSCRIPTIONS; ++index)
        if (s_link.subscriptions[index].filter)
            esp_mqtt_client_subscribe(s_link.client, s_link.subscriptions[index].filter, s_link.subscriptions[index].qos);
    return true;
}

bool wasm_platform_link_down(void *client)
{
    if (!wasm_platform_link_current(client)) return false;
    s_link.connected = false;
    return true;
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

/* 应用停止：输出回到安全电平，取消它的定时器，替它发出遗嘱并断开连接，忘掉它的订阅。 */
void wasm_port_release(void)
{
    for (unsigned index = 0; index < s_pin_count; ++index) pin_safe(index); /* 先管物理输出 */
    for (unsigned id = 0; id < WASM_TIMERS; ++id) wasm_port_timer_cancel(id);
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

bool wasm_platform_init(const char *device_id)
{
    /* 业务连接的客户端 ID 不能用设备 UUID：系统 MQTT 已经在用，同一 Broker 上会互相挤下线。 */
    snprintf(s_link.client_id, sizeof s_link.client_id, "app-%s", device_id ? device_id : "");
    memset(s_pwm_timer, PWM_NONE, sizeof s_pwm_timer);
    for (unsigned id = 0; id < WASM_TIMERS; ++id) {
        const esp_timer_create_args_t timer = {.callback = timer_fired, .arg = &s_timers[id], .name = "app_timer"};
        if (!s_timers[id].handle && esp_timer_create(&timer, &s_timers[id].handle) != ESP_OK) return false;
    }
    return true;
}
