// SPDX-License-Identifier: Apache-2.0
// 本机运行：用设备上的同一份宿主核心（firmware/wasm/base_wasm/core）和 WAMR 运行一个应用，平台服务换成
// 这里的模拟实现。应用包按设备的规则解析和验收（清单、芯片、内存上限、引脚），不验签。
//
// 时间是虚拟的：定时器只在 advance 命令推进时钟时到期，应用睡眠直接把时钟拨快，所以运行结果是确定的。
//
//   app_sim <应用包或 .wasm> [--chip 型号] [--device-id UUID] [--param 键=值]...
//           [--memory 字节] [--stack 字节] [--pin 引脚=用途]...        后三项只用于裸的 .wasm
//
// 命令从标准输入读，一行一个 JSON 对象；发生的事逐行写到标准输出，每条命令以 {"event":"idle",…} 结束。
// 协议的说明和驱动它的 Python 封装见 tools/esp_base_sim.py。
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wasm_host.h"
#include "wasm_package.h"
#include "wasm_port.h"

#define SLOT_BYTES 32768u
#define MEMORY_MAX_BYTES (40u * 1024u) /* 与设备相同：清单里 memory 的上限 */
#define LINEAR_MAX_BYTES (16u * 1024u)
#define BUDGET 2000000u
#define SUBSCRIPTIONS 4
#define PWM_COMBINATIONS 4 /* LEDC 定时器的个数：频率和分辨率的不同组合最多这么多 */
#define PARAMS 16
#define LINE_BYTES 8192
#define ADVANCE_EVENTS_MAX 100000
#define DEFAULT_DEVICE_ID "00000000-0000-4000-8000-000000000001"

static struct {
    uint8_t *image;
    uint32_t image_bytes;
    wasm_package_t package; /* 清单：内存上限、操作数栈、引脚 */
    const char *chip, *device_id;
    struct {
        const char *key, *value;
    } params[PARAMS];
    int param_count;
    uint64_t now_ms, unix_base_ms;
    bool clock_set;
    struct {
        bool active;
        uint64_t due_ms;
        uint32_t period_ms;
    } timers[WASM_TIMERS];
    bool wanted, connected; /* 应用要求过连接；连接此刻是通的 */
    struct {
        bool set;
        char topic[WASM_TOPIC_BYTES + 1];
        uint8_t payload[WASM_PAYLOAD_BYTES];
        size_t topic_length, payload_length;
        int qos;
        bool retain;
    } will;
    char filters[SUBSCRIPTIONS][WASM_TOPIC_BYTES + 1];
    int levels[WASM_PACKAGE_PINS]; /* 与清单里的引脚对应：引脚上的电平 */
    struct {
        uint32_t hz, duty;
        unsigned bits; /* 0：还没配置 */
    } pwm[WASM_PACKAGE_PINS];
    struct {
        bool active;
        uint64_t due_ms;
    } pulses[WASM_PACKAGE_PINS]; /* 有时限的输出 */
    unsigned expired_pins;       /* 时限到了、还没告诉应用的引脚（位 n 是引脚表的第 n 项） */
} sim;

/* ---------- 输出 ---------- */

/* JSON 字符串：转义引号、反斜杠和控制字符；不是合法 UTF-8 的字节换成 U+FFFD。 */
static void put_text(const char *text, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)text;
    putchar('"');
    for (size_t at = 0; at < length;) {
        const unsigned char c = bytes[at];
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
            ++at;
        } else if (c < 0x20 || c == 0x7F) {
            printf("\\u%04x", c);
            ++at;
        } else if (c < 0x80) {
            putchar(c);
            ++at;
        } else {
            /* 多字节字符：首字节决定后面跟几个续字节，续字节都要在串里。 */
            const size_t more = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
            bool valid = more > 0 && at + more < length;
            for (size_t next = 1; valid && next <= more; ++next) valid = (bytes[at + next] & 0xC0) == 0x80;
            if (!valid) {
                fputs("\\ufffd", stdout);
                ++at;
            } else {
                fwrite(bytes + at, 1, more + 1, stdout);
                at += more + 1;
            }
        }
    }
    putchar('"');
}

static void put_hex(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    putchar('"');
    for (size_t at = 0; at < length; ++at) printf("%02x", bytes[at]);
    putchar('"');
}

/* 一行不带字符串的事件。 */
static void emit(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    putchar('\n');
}

static void emit_error(const char *text)
{
    fputs("{\"event\":\"error\",\"text\":", stdout);
    put_text(text, strlen(text));
    puts("}");
}

static const char *mode_name(unsigned mode)
{
    static const char *const names[] = {"in", "in_pullup", "in_pulldown", "out0", "out1", "pwm"};
    return mode < sizeof names / sizeof names[0] ? names[mode] : "?";
}

/* ---------- 平台服务（core/wasm_port.h）的模拟实现 ---------- */

void wasm_port_log(int level, const char *text, size_t length)
{
    printf("{\"event\":\"log\",\"level\":%d,\"text\":", level);
    put_text(text, length);
    puts("}");
}

uint64_t wasm_port_uptime_ms(void)
{
    return sim.now_ms;
}

uint64_t wasm_port_unix_ms(void)
{
    return sim.clock_set ? sim.unix_base_ms + sim.now_ms : 0;
}

int wasm_port_timer_set(unsigned id, uint32_t delay_ms, uint32_t period_ms)
{
    sim.timers[id].active = true;
    sim.timers[id].due_ms = sim.now_ms + delay_ms;
    sim.timers[id].period_ms = period_ms;
    emit("{\"event\":\"timer\",\"id\":%u,\"delay_ms\":%u,\"period_ms\":%u}", id, (unsigned)delay_ms, (unsigned)period_ms);
    return WASM_OK;
}

int wasm_port_timer_cancel(unsigned id)
{
    if (sim.timers[id].active) emit("{\"event\":\"timer_cancel\",\"id\":%u}", id);
    sim.timers[id].active = false;
    return WASM_OK;
}

/* 时限到了的输出回到安全电平。设备上这一步由宿主的定时器直接做，不等应用：应用正在睡眠或处理别的事件时也照常
 * 发生，所以时钟每次前进都要查。应用稍后才收到到期事件（deliver_expired）。 */
static void expire_pulses(void)
{
    for (int index = 0; index < sim.package.pin_count; ++index) {
        if (!sim.pulses[index].active || sim.pulses[index].due_ms > sim.now_ms) continue;
        sim.pulses[index].active = false;
        sim.levels[index] = sim.package.pins[index].mode == WASM_PIN_OUT1;
        sim.expired_pins |= 1u << index;
        emit("{\"event\":\"pulse_expired\",\"pin\":%u,\"level\":%d,\"at_ms\":%llu}", (unsigned)sim.package.pins[index].pin,
             sim.levels[index], (unsigned long long)sim.pulses[index].due_ms);
    }
}

void wasm_port_sleep_ms(uint32_t ms)
{
    emit("{\"event\":\"sleep\",\"ms\":%u}", (unsigned)ms);
    sim.now_ms += ms;
    expire_pulses();
}

static void put_publish(const char *topic, size_t topic_length, const void *payload, size_t payload_length, int qos, bool retain,
                        bool will)
{
    fputs("{\"event\":\"publish\",\"topic\":", stdout);
    put_text(topic, topic_length);
    fputs(",\"payload\":", stdout);
    put_hex(payload, payload_length);
    printf(",\"qos\":%d,\"retain\":%s,\"will\":%s}\n", qos, retain ? "true" : "false", will ? "true" : "false");
}

int wasm_port_mqtt_connect(const wasm_port_will_t *will, unsigned keepalive_s)
{
    memset(&sim.will, 0, sizeof sim.will);
    if (will->topic) {
        sim.will.set = true;
        memcpy(sim.will.topic, will->topic, will->topic_length);
        sim.will.topic_length = will->topic_length;
        if (will->payload_length) memcpy(sim.will.payload, will->payload, will->payload_length);
        sim.will.payload_length = will->payload_length;
        sim.will.qos = will->qos;
        sim.will.retain = will->retain;
    }
    /* 设备上重新 connect 会断开现有的连接再连：这里也回到未连接，等 mqtt_up 命令。 */
    sim.wanted = true;
    sim.connected = false;
    printf("{\"event\":\"connect\",\"keepalive_s\":%u,\"will\":", keepalive_s);
    if (sim.will.set) {
        fputs("{\"topic\":", stdout);
        put_text(sim.will.topic, sim.will.topic_length);
        fputs(",\"payload\":", stdout);
        put_hex(sim.will.payload, sim.will.payload_length);
        printf(",\"qos\":%d,\"retain\":%s}}\n", sim.will.qos, sim.will.retain ? "true" : "false");
    } else {
        puts("null}");
    }
    return WASM_OK;
}

int wasm_port_mqtt_disconnect(void)
{
    sim.wanted = sim.connected = false;
    emit("{\"event\":\"disconnect\"}");
    return WASM_OK;
}

int wasm_port_mqtt_subscribe(const char *filter, size_t length, int qos)
{
    for (int index = 0; index < SUBSCRIPTIONS; ++index)
        if (strlen(sim.filters[index]) == length && !memcmp(sim.filters[index], filter, length)) return WASM_OK;
    for (int index = 0; index < SUBSCRIPTIONS; ++index) {
        if (sim.filters[index][0]) continue;
        memcpy(sim.filters[index], filter, length);
        sim.filters[index][length] = '\0';
        fputs("{\"event\":\"subscribe\",\"filter\":", stdout);
        put_text(filter, length);
        printf(",\"qos\":%d}\n", qos);
        return WASM_OK;
    }
    return WASM_E_NO_MEMORY; /* 订阅槽用完了 */
}

int wasm_port_mqtt_publish(const char *topic, size_t topic_length, const void *payload, size_t payload_length, int qos,
                           bool retain)
{
    if (!sim.connected) return WASM_E_NOT_READY;
    put_publish(topic, topic_length, payload, payload_length, qos, retain, false);
    return WASM_OK;
}

static int copy_text(const char *text, char *out, size_t capacity)
{
    const size_t length = strlen(text);
    if (length > capacity) return WASM_E_TOO_LARGE;
    memcpy(out, text, length);
    return (int)length;
}

int wasm_port_param(const char *key, char *out, size_t capacity)
{
    for (int index = 0; index < sim.param_count; ++index)
        if (!strcmp(sim.params[index].key, key)) return copy_text(sim.params[index].value, out, capacity);
    return WASM_E_NOT_FOUND;
}

int wasm_port_info(wasm_info_t what, char *out, size_t capacity)
{
    return copy_text(what == WASM_INFO_DEVICE_ID ? sim.device_id : sim.chip, out, capacity);
}

static int pin_slot(unsigned pin)
{
    for (int index = 0; index < sim.package.pin_count; ++index)
        if (sim.package.pins[index].pin == pin) return index;
    return -1;
}

int wasm_port_gpio_write(unsigned pin, int level)
{
    const int index = pin_slot(pin);
    if (index < 0) return WASM_E_INVALID;
    sim.pulses[index].active = false; /* 应用自己接管了这个引脚：之前的时限作废 */
    sim.expired_pins &= ~(1u << index);
    sim.levels[index] = level != 0;
    emit("{\"event\":\"gpio\",\"pin\":%u,\"level\":%d}", pin, sim.levels[index]);
    return WASM_OK;
}

int wasm_port_gpio_pulse(unsigned pin, int level, uint32_t ms)
{
    const int index = pin_slot(pin);
    if (index < 0) return WASM_E_INVALID;
    sim.pulses[index].active = true;
    sim.pulses[index].due_ms = sim.now_ms + ms;
    sim.expired_pins &= ~(1u << index);
    sim.levels[index] = level != 0;
    emit("{\"event\":\"pulse\",\"pin\":%u,\"level\":%d,\"ms\":%u}", pin, sim.levels[index], (unsigned)ms);
    return WASM_OK;
}

int wasm_port_gpio_read(unsigned pin)
{
    const int index = pin_slot(pin);
    return index < 0 ? WASM_E_INVALID : sim.levels[index];
}

/* 这个频率配不配得出这样的分辨率：LEDC 的分频系数（时钟 ÷ 频率 ÷ 2^位数）要在 1 到 1024 之间，几个可选的时钟里
 * 有一个行就行。设备上所有定时器还得共用同一个时钟（ESP32 除外），这一条这里不模拟，以设备为准。 */
static bool pwm_feasible(uint32_t hz, unsigned bits)
{
    static const uint32_t c3[] = {80000000, 40000000, 17500000}, esp32[] = {80000000, 8000000, 1000000};
    const uint32_t *clocks = !strcmp(sim.chip, "esp32") ? esp32 : c3;
    for (int index = 0; index < 3; ++index) {
        const double divider = (double)clocks[index] / ((double)hz * (double)(1u << bits));
        if (divider >= 1.0 && divider < 1024.0) return true;
    }
    return false;
}

int wasm_port_pwm_config(unsigned pin, uint32_t frequency_hz, unsigned resolution_bits)
{
    const int index = pin_slot(pin);
    if (index < 0) return WASM_E_INVALID;
    if (!pwm_feasible(frequency_hz, resolution_bits)) {
        sim.pwm[index].bits = 0;
        return WASM_E_INVALID;
    }
    /* 频率和分辨率相同的引脚共用一个定时器：数一数除了这个引脚之外已经有几种组合。 */
    int combinations = 0;
    bool shared = false;
    for (int other = 0; other < sim.package.pin_count; ++other) {
        if (other == index || !sim.pwm[other].bits) continue;
        if (sim.pwm[other].hz == frequency_hz && sim.pwm[other].bits == resolution_bits) shared = true;
        bool counted = false;
        for (int earlier = 0; earlier < other && !counted; ++earlier)
            counted = earlier != index && sim.pwm[earlier].bits && sim.pwm[earlier].hz == sim.pwm[other].hz &&
                      sim.pwm[earlier].bits == sim.pwm[other].bits;
        combinations += !counted;
    }
    if (!shared && combinations >= PWM_COMBINATIONS) {
        sim.pwm[index].bits = 0;
        return WASM_E_NO_MEMORY;
    }
    sim.pwm[index].hz = frequency_hz;
    sim.pwm[index].bits = resolution_bits;
    sim.pwm[index].duty = 0;
    emit("{\"event\":\"pwm_config\",\"pin\":%u,\"hz\":%u,\"bits\":%u}", pin, (unsigned)frequency_hz, resolution_bits);
    return WASM_OK;
}

int wasm_port_pwm_write(unsigned pin, uint32_t duty)
{
    const int index = pin_slot(pin);
    if (index < 0 || !sim.pwm[index].bits) return WASM_E_NOT_READY;
    sim.pwm[index].duty = duty;
    emit("{\"event\":\"pwm\",\"pin\":%u,\"duty\":%u}", pin, (unsigned)duty);
    return WASM_OK;
}

int wasm_port_pwm_read(unsigned pin)
{
    const int index = pin_slot(pin);
    return index < 0 || !sim.pwm[index].bits ? WASM_E_NOT_READY : (int)sim.pwm[index].duty;
}

/* 把清单里的引脚都置到安全状态：输出回到安全电平，PWM 停下。输入脚上的电平是外面给的，不动。 */
static void pins_safe(void)
{
    for (int index = 0; index < sim.package.pin_count; ++index) {
        const unsigned mode = sim.package.pins[index].mode;
        memset(&sim.pwm[index], 0, sizeof sim.pwm[index]);
        sim.pulses[index].active = false;
        sim.expired_pins &= ~(1u << index);
        if (mode == WASM_PIN_OUT0 || mode == WASM_PIN_OUT1 || mode == WASM_PIN_PWM) sim.levels[index] = mode == WASM_PIN_OUT1;
        emit("{\"event\":\"safe\",\"pin\":%u,\"mode\":\"%s\",\"level\":%d}", (unsigned)sim.package.pins[index].pin,
             mode_name(mode), sim.levels[index]);
    }
}

void wasm_port_release(void)
{
    pins_safe();
    for (unsigned id = 0; id < WASM_TIMERS; ++id) sim.timers[id].active = false;
    /* 主动断开不会触发遗嘱：宿主替应用把它发出去。 */
    if (sim.connected && sim.will.set)
        put_publish(sim.will.topic, sim.will.topic_length, sim.will.payload, sim.will.payload_length, sim.will.qos,
                    sim.will.retain, true);
    if (sim.wanted) emit("{\"event\":\"disconnect\"}");
    sim.wanted = sim.connected = false;
    memset(&sim.will, 0, sizeof sim.will);
    memset(sim.filters, 0, sizeof sim.filters);
}

/* ---------- 命令 ---------- */

static bool read_image(void *context, uint8_t *out, uint32_t bytes)
{
    (void)context;
    memcpy(out, sim.image, bytes);
    return true;
}

static void put_stats(const char *event)
{
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    emit("{\"event\":\"%s\",\"pool_used\":%u,\"pool_peak\":%u,\"linear\":%u,\"calls\":%u,\"faults\":%u}", event,
         (unsigned)stats.pool_used, (unsigned)stats.pool_peak, (unsigned)stats.linear_bytes, (unsigned)stats.calls,
         (unsigned)stats.faults);
}

static bool s_loaded; /* 应用启动成功过、还没停：出了故障的应用也算，停的时候要报一声 */

static void stop(void)
{
    wasm_host_stop();
    if (s_loaded) emit("{\"event\":\"stopped\"}");
    s_loaded = false;
}

static void start(void)
{
    char error[160] = "";
    if (wasm_host_running()) {
        emit_error("the app is already running");
        return;
    }
    /* 本机的指针是 64 位，运行时自己的结构比设备上大：内存池按清单的 1.5 倍给，免得设备上放得下的应用在这里
     * 放不下。设备上的实际用量看 app/state 里的 pool_used、pool_peak。 */
    const uint32_t pool = sizeof(void *) > 4 ? sim.package.memory_bytes + sim.package.memory_bytes / 2 : sim.package.memory_bytes;
    const wasm_host_limits_t limits = {.pool_bytes = pool, .stack_bytes = sim.package.stack_bytes,
                                       .linear_max_bytes = LINEAR_MAX_BYTES, .budget = BUDGET, .pins = sim.package.pins,
                                       .pin_count = sim.package.pin_count};
    pins_safe(); /* 应用的代码运行之前，引脚先到安全状态 */
    if (!wasm_host_start(read_image, NULL, sim.image_bytes, &limits, error, sizeof error)) {
        fputs("{\"event\":\"start_failed\",\"error\":", stdout);
        put_text(error, strlen(error));
        puts("}");
        return;
    }
    s_loaded = true;
    put_stats("started");
}

/* 把一个事件交给应用；出了故障就停掉它（设备上随后会退避重启，这里停着等 start 命令）。 */
static void dispatch(wasm_event_kind_t kind, uint32_t a, uint32_t b)
{
    if (!wasm_host_running() || wasm_host_event(kind, a, b)) return;
    const char *reason = wasm_host_fault();
    fputs("{\"event\":\"fault\",\"reason\":", stdout);
    put_text(reason, strlen(reason));
    puts("}");
    stop();
}

/* 把时限到了的引脚告诉应用。只在两次调用之间做：应用一次只处理一个事件。 */
static void deliver_expired(void)
{
    for (int index = 0; index < sim.package.pin_count && sim.expired_pins; ++index) {
        if (!(sim.expired_pins & 1u << index)) continue;
        sim.expired_pins &= ~(1u << index);
        dispatch(WASM_EVENT_PIN_EXPIRED, sim.package.pins[index].pin, 0);
    }
}

static bool topic_matches(const char *filter, const char *topic)
{
    while (*filter) {
        if (*filter == '#') return true;
        if (*filter == '/' && filter[1] == '#' && !*topic) return true; /* a/# 也匹配 a */
        if (*filter == '+') {
            while (*topic && *topic != '/') ++topic;
            ++filter;
            continue;
        }
        if (*filter != *topic) return false;
        ++filter;
        ++topic;
    }
    return !*topic;
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void message(const cJSON *json)
{
    static uint8_t payload[WASM_PAYLOAD_BYTES];
    const cJSON *topic = cJSON_GetObjectItemCaseSensitive(json, "topic");
    const cJSON *hex = cJSON_GetObjectItemCaseSensitive(json, "payload");
    if (!cJSON_IsString(topic) || !cJSON_IsString(hex) || strlen(hex->valuestring) % 2) {
        emit_error("message needs a topic and a hex payload");
        return;
    }
    const size_t topic_length = strlen(topic->valuestring), payload_length = strlen(hex->valuestring) / 2;
    const char *dropped = NULL;
    /* 与设备相同：没连上、没有订阅匹配、超长的消息到不了应用。 */
    if (!sim.connected) dropped = "not_connected";
    else if (topic_length == 0 || topic_length > WASM_TOPIC_BYTES || payload_length > WASM_PAYLOAD_BYTES) dropped = "too_large";
    else {
        bool matched = false;
        for (int index = 0; index < SUBSCRIPTIONS && !matched; ++index)
            matched = sim.filters[index][0] && topic_matches(sim.filters[index], topic->valuestring);
        if (!matched) dropped = "not_subscribed";
    }
    if (dropped) {
        emit("{\"event\":\"dropped\",\"reason\":\"%s\"}", dropped);
        return;
    }
    for (size_t at = 0; at < payload_length; ++at) {
        const int high = hex_value(hex->valuestring[at * 2]), low = hex_value(hex->valuestring[at * 2 + 1]);
        if (high < 0 || low < 0) {
            emit_error("the payload is not hex");
            return;
        }
        payload[at] = (uint8_t)(high << 4 | low);
    }
    wasm_host_event_data(topic->valuestring, topic_length, payload, payload_length);
    dispatch(WASM_EVENT_MQTT_MESSAGE, (uint32_t)topic_length, (uint32_t)payload_length);
}

/* 把时钟推进 ms 毫秒，期间到期的定时器和有时限的输出按到期的先后处理。 */
static void advance(uint64_t ms)
{
    uint64_t target = sim.now_ms + ms;
    for (int events = 0;; ++events) {
        /* 下一个到期的：定时器（只有应用在运行时才有意义）或有时限的输出；同时到期时先处理输出。 */
        int timer = -1, pulse = -1;
        for (int id = 0; id < WASM_TIMERS && wasm_host_running(); ++id)
            if (sim.timers[id].active && sim.timers[id].due_ms <= target && (timer < 0 || sim.timers[id].due_ms < sim.timers[timer].due_ms))
                timer = id;
        for (int index = 0; index < sim.package.pin_count; ++index)
            if (sim.pulses[index].active && sim.pulses[index].due_ms <= target && (pulse < 0 || sim.pulses[index].due_ms < sim.pulses[pulse].due_ms))
                pulse = index;
        if (timer < 0 && pulse < 0) break;
        if (events >= ADVANCE_EVENTS_MAX) {
            emit_error("too many timer events in one advance");
            break;
        }
        if (pulse >= 0 && (timer < 0 || sim.pulses[pulse].due_ms <= sim.timers[timer].due_ms)) {
            if (sim.pulses[pulse].due_ms > sim.now_ms) sim.now_ms = sim.pulses[pulse].due_ms;
            expire_pulses();
        } else {
            if (sim.timers[timer].due_ms > sim.now_ms) sim.now_ms = sim.timers[timer].due_ms;
            if (sim.timers[timer].period_ms) sim.timers[timer].due_ms += sim.timers[timer].period_ms;
            else sim.timers[timer].active = false;
            dispatch(WASM_EVENT_TIMER, (uint32_t)timer, 0);
        }
        deliver_expired();
        if (sim.now_ms > target) target = sim.now_ms; /* 应用在处理里睡过了头 */
    }
    if (target > sim.now_ms) sim.now_ms = target;
}

static double number(const cJSON *json, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static void command(const cJSON *json)
{
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json, "cmd");
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : "";
    if (!strcmp(name, "start")) {
        start();
    } else if (!strcmp(name, "stop")) {
        stop();
    } else if (!strcmp(name, "mqtt_up")) {
        /* 设备只在应用要求过连接之后才会去连。 */
        if (!sim.wanted) {
            emit_error("the app has not asked for a connection");
        } else if (!sim.connected) {
            sim.connected = true;
            dispatch(WASM_EVENT_MQTT_UP, 0, 0);
        }
    } else if (!strcmp(name, "mqtt_down")) {
        if (sim.connected) {
            sim.connected = false;
            dispatch(WASM_EVENT_MQTT_DOWN, 0, 0);
        }
    } else if (!strcmp(name, "message")) {
        message(json);
    } else if (!strcmp(name, "advance")) {
        const double ms = number(json, "ms", -1);
        if (ms < 0 || ms > 86400000.0 * 365) emit_error("advance needs ms");
        else advance((uint64_t)ms);
    } else if (!strcmp(name, "input")) {
        /* 外面给输入脚的电平。 */
        const int index = pin_slot((unsigned)number(json, "pin", 255));
        const unsigned mode = index < 0 ? WASM_PIN_PWM : sim.package.pins[index].mode;
        if (mode != WASM_PIN_IN && mode != WASM_PIN_IN_PULLUP && mode != WASM_PIN_IN_PULLDOWN) emit_error("not an input pin");
        else sim.levels[index] = number(json, "level", 0) != 0;
    } else if (!strcmp(name, "clock")) {
        /* 对时：之后 unix_ms 返回这个时刻加上流逝的时间。 */
        sim.unix_base_ms = (uint64_t)number(json, "unix_ms", 0) - sim.now_ms;
        sim.clock_set = true;
    } else if (!strcmp(name, "stats")) {
        put_stats("stats");
    } else if (!strcmp(name, "event")) {
        /* 原样交给应用一个事件：测应用怎样对待没设过的定时器、不认识的事件种类。 */
        dispatch((wasm_event_kind_t)number(json, "kind", 0), (uint32_t)number(json, "a", 0), (uint32_t)number(json, "b", 0));
    } else {
        emit_error("unknown command");
    }
}

/* ---------- 启动 ---------- */

static int usage(const char *problem)
{
    fprintf(stderr, "%s\nusage: app_sim <app.ebapp | app.wasm> [--chip esp32c3|esp32] [--device-id UUID] [--param key=value]...\n"
                    "               [--memory bytes] [--stack bytes] [--pin number=mode]...   (the last three: bare .wasm only)\n",
            problem);
    return 2;
}

static bool load(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    const bool ok = data && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!ok) {
        free(data);
        return false;
    }
    sim.image = data;
    sim.image_bytes = (uint32_t)size;
    return true;
}

static int reject(const char *error)
{
    printf("{\"event\":\"rejected\",\"error\":\"%s\"}\n", error);
    return 2;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *path = NULL;
    uint32_t memory = 16384, stack = 2048;
    wasm_pin_t pins[WASM_PACKAGE_PINS];
    int pin_count = 0;
    sim.chip = "esp32c3";
    sim.device_id = DEFAULT_DEVICE_ID;
    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index], *value = index + 1 < argc ? argv[index + 1] : NULL;
        if (option[0] != '-') {
            if (path) return usage("only one app can be given");
            path = option;
            continue;
        }
        if (!value) return usage("an option is missing its value");
        ++index;
        if (!strcmp(option, "--chip")) {
            sim.chip = value;
        } else if (!strcmp(option, "--device-id")) {
            sim.device_id = value;
        } else if (!strcmp(option, "--memory")) {
            memory = (uint32_t)strtoul(value, NULL, 10);
        } else if (!strcmp(option, "--stack")) {
            stack = (uint32_t)strtoul(value, NULL, 10);
        } else if (!strcmp(option, "--param")) {
            char *separator = strchr(value, '=');
            if (!separator || sim.param_count >= PARAMS) return usage("--param takes key=value, at most 16 of them");
            *separator = '\0';
            sim.params[sim.param_count].key = value;
            sim.params[sim.param_count++].value = separator + 1;
        } else if (!strcmp(option, "--pin")) {
            const char *separator = strchr(value, '=');
            int mode = -1;
            for (int candidate = 0; separator && candidate <= WASM_PIN_PWM && mode < 0; ++candidate)
                if (!strcmp(separator + 1, mode_name((unsigned)candidate))) mode = candidate;
            if (mode < 0 || pin_count >= WASM_PACKAGE_PINS) return usage("--pin takes number=mode, at most 8 of them");
            pins[pin_count++] = (wasm_pin_t){.pin = (uint8_t)atoi(value), .mode = (uint8_t)mode};
        } else {
            return usage("unknown option");
        }
    }
    if (!path) return usage("no app given");
    if (!load(path)) return usage("the app cannot be read");

    if (sim.image_bytes >= 4 && !memcmp(sim.image, WASM_PACKAGE_MAGIC, 4)) {
        /* 应用包：清单和验收规则与设备相同；签名不在这里核对。 */
        const char *error = wasm_package_parse(sim.image, sim.image_bytes, SLOT_BYTES, &sim.package);
        if (!error && (uint64_t)sim.package.wasm_offset + sim.package.wasm_bytes > sim.image_bytes) error = "package_invalid";
        if (error) return reject(error);
        memmove(sim.image, sim.image + sim.package.wasm_offset, sim.package.wasm_bytes);
        sim.image_bytes = sim.package.wasm_bytes;
    } else {
        /* 裸的 .wasm：清单由命令行给出。 */
        if (memory < WASM_PACKAGE_MEMORY_MIN || memory > WASM_PACKAGE_MEMORY_MAX || stack < WASM_PACKAGE_STACK_MIN ||
            stack > WASM_PACKAGE_STACK_MAX)
            return reject("manifest_invalid");
        snprintf(sim.package.name, sizeof sim.package.name, "app");
        snprintf(sim.package.version, sizeof sim.package.version, "0.0.0+0");
        sim.package.abi = WASM_ABI;
        sim.package.memory_bytes = memory;
        sim.package.stack_bytes = stack;
        for (int index = 0; index < pin_count; ++index) {
            if (wasm_pin_mode(sim.package.pins, sim.package.pin_count, pins[index].pin) >= 0) return reject("manifest_invalid");
            sim.package.pins[sim.package.pin_count++] = pins[index];
        }
    }
    const char *error = wasm_package_accept(&sim.package, sim.chip, MEMORY_MAX_BYTES);
    if (error) return reject(error);
    /* 带上拉的输入脚没人驱动时是高电平。 */
    for (int index = 0; index < sim.package.pin_count; ++index) sim.levels[index] = sim.package.pins[index].mode == WASM_PIN_IN_PULLUP;

    fputs("{\"event\":\"ready\",\"name\":", stdout);
    put_text(sim.package.name, strlen(sim.package.name));
    fputs(",\"version\":", stdout);
    put_text(sim.package.version, strlen(sim.package.version));
    printf(",\"abi\":%u,\"memory\":%u,\"stack\":%u,\"wasm_bytes\":%u,\"chip\":\"%s\"}\n", (unsigned)sim.package.abi,
           (unsigned)sim.package.memory_bytes, (unsigned)sim.package.stack_bytes, (unsigned)sim.image_bytes, sim.chip);

    static char line[LINE_BYTES];
    while (fgets(line, sizeof line, stdin)) {
        cJSON *json = cJSON_Parse(line);
        if (cJSON_IsObject(json)) command(json);
        else emit_error("a command is one JSON object per line");
        cJSON_Delete(json);
        deliver_expired(); /* 应用在这条命令里睡过去的时间里到期的输出 */
        emit("{\"event\":\"idle\",\"running\":%s,\"uptime_ms\":%llu}", wasm_host_running() ? "true" : "false",
             (unsigned long long)sim.now_ms);
    }
    wasm_host_stop();
    free(sim.image);
    return 0;
}
