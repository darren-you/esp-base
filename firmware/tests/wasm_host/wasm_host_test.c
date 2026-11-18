// SPDX-License-Identifier: Apache-2.0
// host 测试：宿主核心加载真实的应用（tests/apps/*.wasm，源码在 sdk/rust），核对宿主接口、事件分发，
// 以及每一类应用故障都只停掉应用：陷阱、死循环、栈耗尽、越界指针、缺少的宿主接口、没有收缩的线性内存。
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_host.h"
#include "wasm_port.h"

static int s_failures, s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { ++s_failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

/* ---------- 平台服务的桩：只记录调用 ---------- */

#define RECORDS 16
#define MOCK_PINS 48
#define DEVICE_ID "00000000-0000-4000-8000-000000000001"
typedef struct {
    char topic[WASM_TOPIC_BYTES + 1];
    char payload[256];
    size_t payload_length;
    int qos;
    bool retain;
} publish_t;

static struct {
    int logs, releases, sleeps;
    char log[128];
    int published;
    publish_t publishes[RECORDS], last;
    bool connect_wanted, connected;
    publish_t will;
    bool will_set;
    unsigned keepalive_s;
    int subscriptions;
    char filters[4][WASM_TOPIC_BYTES + 1];
    struct { bool active; uint32_t delay_ms, period_ms; } timers[WASM_TIMERS];
    uint64_t uptime_ms, unix_ms;
    int levels[MOCK_PINS], gpio_writes, pwm_configs;
    uint32_t pulse_ms[MOCK_PINS]; /* 有时限的输出：最近一次的时限 */
    struct { uint32_t hz, duty; unsigned bits; } pwm[MOCK_PINS];
} m;

void wasm_port_log(int level, const char *text, size_t length)
{
    (void)level;
    ++m.logs;
    snprintf(m.log, sizeof m.log, "%.*s", (int)length, text);
}

uint64_t wasm_port_uptime_ms(void) { return m.uptime_ms; }
uint64_t wasm_port_unix_ms(void) { return m.unix_ms; }

int wasm_port_timer_set(unsigned id, uint32_t delay_ms, uint32_t period_ms)
{
    m.timers[id].active = true;
    m.timers[id].delay_ms = delay_ms;
    m.timers[id].period_ms = period_ms;
    return WASM_OK;
}

int wasm_port_timer_cancel(unsigned id)
{
    m.timers[id].active = false;
    return WASM_OK;
}

void wasm_port_sleep_ms(uint32_t ms)
{
    ++m.sleeps;
    m.uptime_ms += ms;
}

static void record(publish_t *out, const char *topic, size_t topic_length, const void *payload, size_t payload_length,
                   int qos, bool retain)
{
    memset(out, 0, sizeof *out);
    snprintf(out->topic, sizeof out->topic, "%.*s", (int)topic_length, topic);
    out->payload_length = payload_length < sizeof out->payload - 1 ? payload_length : sizeof out->payload - 1;
    if (out->payload_length) memcpy(out->payload, payload, out->payload_length);
    out->qos = qos;
    out->retain = retain;
}

int wasm_port_mqtt_connect(const wasm_port_will_t *will, unsigned keepalive_s)
{
    m.connect_wanted = true;
    m.keepalive_s = keepalive_s;
    m.will_set = will->topic != NULL;
    if (will->topic) record(&m.will, will->topic, will->topic_length, will->payload, will->payload_length, will->qos, will->retain);
    return WASM_OK;
}

int wasm_port_mqtt_disconnect(void)
{
    m.connect_wanted = false;
    return WASM_OK;
}

int wasm_port_mqtt_subscribe(const char *filter, size_t length, int qos)
{
    (void)qos;
    if (m.subscriptions >= 4) return WASM_E_NO_MEMORY;
    snprintf(m.filters[m.subscriptions++], sizeof m.filters[0], "%.*s", (int)length, filter);
    return WASM_OK;
}

int wasm_port_mqtt_publish(const char *topic, size_t topic_length, const void *payload, size_t payload_length, int qos,
                           bool retain)
{
    if (!m.connected) return WASM_E_NOT_READY;
    if (m.published < RECORDS) record(&m.publishes[m.published], topic, topic_length, payload, payload_length, qos, retain);
    record(&m.last, topic, topic_length, payload, payload_length, qos, retain);
    ++m.published;
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
    if (!strcmp(key, "hxh_site")) return copy_text("lab", out, capacity);
    if (!strcmp(key, "long_value")) return copy_text("0123456789abcdefg", out, capacity); /* 比夹具的缓冲多一个字节 */
    return WASM_E_NOT_FOUND;
}

int wasm_port_info(wasm_info_t what, char *out, size_t capacity)
{
    return copy_text(what == WASM_INFO_DEVICE_ID ? DEVICE_ID : "esp32c3", out, capacity);
}

/* 宿主核心只该把清单里声明过的引脚交给平台。 */
static unsigned declared(unsigned pin)
{
    if (pin >= MOCK_PINS) {
        fprintf(stderr, "the port was handed pin %u\n", pin);
        abort();
    }
    return pin;
}

int wasm_port_gpio_write(unsigned pin, int level)
{
    m.levels[declared(pin)] = level;
    ++m.gpio_writes;
    return WASM_OK;
}

int wasm_port_gpio_read(unsigned pin) { return m.levels[declared(pin)]; }

int wasm_port_gpio_pulse(unsigned pin, int level, uint32_t ms)
{
    m.levels[declared(pin)] = level;
    m.pulse_ms[pin] = ms;
    ++m.gpio_writes;
    return WASM_OK;
}

int wasm_port_pwm_config(unsigned pin, uint32_t frequency_hz, unsigned resolution_bits)
{
    if (frequency_hz == 39999) return WASM_E_INVALID; /* 桩：这个频率配不出来 */
    m.pwm[declared(pin)].hz = frequency_hz;
    m.pwm[pin].bits = resolution_bits;
    m.pwm[pin].duty = 0;
    ++m.pwm_configs;
    return WASM_OK;
}

int wasm_port_pwm_write(unsigned pin, uint32_t duty)
{
    m.pwm[declared(pin)].duty = duty;
    return WASM_OK;
}

int wasm_port_pwm_read(unsigned pin) { return (int)m.pwm[declared(pin)].duty; }

void wasm_port_release(void)
{
    ++m.releases;
    m.connect_wanted = m.connected = false;
    for (unsigned id = 0; id < WASM_TIMERS; ++id) m.timers[id].active = false;
}

/* ---------- 辅助 ---------- */

static const char *s_apps;
static const wasm_host_limits_t LIMITS = {.pool_bytes = 16 * 1024, .stack_bytes = 2048, .linear_max_bytes = 16 * 1024,
                                          .budget = 2000000};

/* 读入应用镜像；调用方在 wasm_host_stop 之后释放。 */
static uint8_t *load(const char *name, uint32_t *bytes)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.wasm", s_apps, name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    uint8_t *image = malloc((size_t)size);
    if (!image || fread(image, 1, (size_t)size, file) != (size_t)size) exit(2);
    fclose(file);
    *bytes = (uint32_t)size;
    return image;
}

/* 宿主核心读入镜像的回调：从文件内容拷进它在内存池里分配的缓冲。 */
static bool copy_image(void *context, uint8_t *out, uint32_t bytes)
{
    memcpy(out, context, bytes);
    return true;
}

static bool unreadable(void *context, uint8_t *out, uint32_t bytes)
{
    (void)context;
    (void)out;
    (void)bytes;
    return false;
}

static bool start(const char *name, const wasm_host_limits_t *limits, uint8_t **image, char *error, size_t error_size)
{
    uint32_t bytes;
    memset(&m, 0, sizeof m);
    *image = load(name, &bytes);
    return wasm_host_start(copy_image, *image, bytes, limits, error, error_size);
}

static void stop(uint8_t *image)
{
    wasm_host_stop();
    free(image);
}

/* 应用启动后的内存用量：给清单里的 memory 取值作参考。 */
static void usage(const char *name)
{
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    printf("  %s: pool used %u, peak %u, linear %u\n", name, (unsigned)stats.pool_used, (unsigned)stats.pool_peak,
           (unsigned)stats.linear_bytes);
}

static bool published(int index, const char *topic, const char *payload)
{
    return index < m.published && !strcmp(m.publishes[index].topic, topic) &&
           m.publishes[index].payload_length == strlen(payload) && !memcmp(m.publishes[index].payload, payload, strlen(payload));
}

/* ---------- 用例 ---------- */

static void test_hello(void)
{
    uint8_t *image;
    char error[128] = "";
    CHECK(start("hello", &LIMITS, &image, error, sizeof error));
    CHECK(wasm_host_running() && !error[0]);
    usage("hello");
    /* app_init：一行日志，要求连接（带遗嘱），订阅，周期定时器。 */
    CHECK(m.logs == 1 && !strcmp(m.log, "hello: init"));
    CHECK(m.connect_wanted && m.keepalive_s == 30 && m.will_set);
    CHECK(!strcmp(m.will.topic, "hello/state") && m.will.payload_length == 7 && !memcmp(m.will.payload, "offline", 7) &&
          m.will.qos == 1 && m.will.retain);
    CHECK(m.subscriptions == 1 && !strcmp(m.filters[0], "hello/in"));
    CHECK(m.timers[0].active && m.timers[0].delay_ms == 1000 && m.timers[0].period_ms == 1000);

    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    /* 线性内存收缩到实际用量：4 KiB 栈加几十字节数据，而不是一整页 64 KiB。 */
    CHECK(stats.linear_bytes > 4096 && stats.linear_bytes < 4096 + 256);
    CHECK(stats.pool_used > 0 && stats.pool_peak >= stats.pool_used && stats.pool_peak < LIMITS.pool_bytes);

    /* 没连上时定时器只计数，不发布。 */
    CHECK(wasm_host_event(WASM_EVENT_TIMER, 0, 0) && m.published == 0);
    m.connected = true;
    CHECK(wasm_host_event(WASM_EVENT_MQTT_UP, 0, 0));
    CHECK(published(0, "hello/state", "online") && m.publishes[0].retain && m.publishes[0].qos == 1);
    CHECK(wasm_host_event(WASM_EVENT_TIMER, 0, 0) && published(1, "hello/ticks", "2"));
    for (int index = 0; index < 9; ++index) CHECK(wasm_host_event(WASM_EVENT_TIMER, 0, 0));
    CHECK(m.published == 11 && published(10, "hello/ticks", "11"));

    /* 消息事件：载荷经 event_payload 读出后回显。 */
    static const char topic[] = "hello/in", payload[] = "{\"ping\":1}";
    wasm_host_event_data(topic, sizeof topic - 1, payload, sizeof payload - 1);
    CHECK(wasm_host_event(WASM_EVENT_MQTT_MESSAGE, sizeof topic - 1, sizeof payload - 1));
    CHECK(published(11, "hello/echo", payload));
    /* 载荷比应用的缓冲大：应用拿到 TooLarge，不回显，也不出故障。 */
    static char large[200];
    memset(large, 'x', sizeof large);
    wasm_host_event_data(topic, sizeof topic - 1, large, sizeof large);
    CHECK(wasm_host_event(WASM_EVENT_MQTT_MESSAGE, sizeof topic - 1, sizeof large) && m.published == 12);
    /* 事件数据只在那一次调用期间有效。 */
    CHECK(wasm_host_event(WASM_EVENT_MQTT_MESSAGE, 0, 0) && m.published == 12);

    m.connected = false;
    CHECK(wasm_host_event(WASM_EVENT_MQTT_DOWN, 0, 0));
    CHECK(wasm_host_event(WASM_EVENT_TIMER, 0, 0) && m.published == 12);
    /* 不认识的事件种类：应用忽略。 */
    CHECK(wasm_host_event((wasm_event_kind_t)99, 1, 2));
    wasm_host_stats(&stats);
    CHECK(stats.faults == 0 && stats.calls == 19);

    stop(image);
    CHECK(m.releases == 1 && !wasm_host_running());
    /* 停止后事件不再送达；再停一次无害。 */
    CHECK(!wasm_host_event(WASM_EVENT_TIMER, 0, 0));
    wasm_host_stop();
}

static void test_restart_does_not_leak(void)
{
    uint32_t first = 0;
    for (int round = 0; round < 20; ++round) {
        uint8_t *image;
        char error[128];
        CHECK(start("hello", &LIMITS, &image, error, sizeof error));
        wasm_host_stats_t stats;
        wasm_host_stats(&stats);
        if (round == 0) first = stats.pool_used;
        CHECK(stats.pool_used == first);
        stop(image);
    }
}

static void expect_fault(const char *name, wasm_event_kind_t kind, const char *reason)
{
    uint8_t *image;
    char error[128] = "";
    CHECK(start(name, &LIMITS, &image, error, sizeof error));
    CHECK(!wasm_host_event(kind, 0, 0));
    if (!strstr(wasm_host_fault(), reason)) fprintf(stderr, "%s fault: %s\n", name, wasm_host_fault());
    CHECK(strstr(wasm_host_fault(), reason) != NULL);
    CHECK(!wasm_host_running());
    /* 出故障后不再调用应用。 */
    CHECK(!wasm_host_event(kind, 0, 0));
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    CHECK(stats.faults == 1);
    stop(image);
    CHECK(m.releases == 1);
}

static void test_faults(void)
{
    /* 死循环：指令预算用完即停。 */
    expect_fault("spin", WASM_EVENT_TIMER, "instruction limit exceeded");
    /* 越界下标：Rust 的 panic 变成陷阱。 */
    expect_fault("trap", WASM_EVENT_TIMER, "unreachable");
    /* 线性内存之外的指针交给宿主接口。 */
    expect_fault("bad_pointer", WASM_EVENT_TIMER, "out of bounds memory access");
    /* 无限递归：先耗尽的是运行时给应用的操作数栈（清单里的栈大小），不是宿主任务的栈。 */
    expect_fault("deep", WASM_EVENT_TIMER, "wasm operand stack overflow");
}

static void test_trap_keeps_other_events(void)
{
    /* 没触发故障的事件照常处理。 */
    uint8_t *image;
    char error[128];
    CHECK(start("trap", &LIMITS, &image, error, sizeof error));
    CHECK(wasm_host_event(WASM_EVENT_MQTT_UP, 0, 0) && m.logs == 1 && !strcmp(m.log, "trap: alive"));
    stop(image);
}

static void expect_start_failure(const char *name, const wasm_host_limits_t *limits, const char *reason)
{
    uint8_t *image;
    char error[128] = "";
    CHECK(!start(name, limits, &image, error, sizeof error));
    if (!strstr(error, reason)) fprintf(stderr, "%s error: %s\n", name, error);
    CHECK(strstr(error, reason) != NULL);
    CHECK(!wasm_host_running());
    free(image);
    /* 失败后可以直接启动别的应用。 */
    CHECK(start("hello", &LIMITS, &image, error, sizeof error));
    stop(image);
}

static void test_start_failures(void)
{
    expect_start_failure("init_fail", &LIMITS, "app_init: returned -1"); /* init 返回的错误码：Error::Invalid */
    expect_start_failure("unknown_import", &LIMITS, "base.not_in_this_firmware is not provided by this firmware");
    expect_start_failure("no_event", &LIMITS, "app_init or app_event is not exported");
    /* 没有导出 __heap_base、__data_end：线性内存是整页 64 KiB，超过上限。 */
    expect_start_failure("hello_unshrunk", &LIMITS, "linear memory 65536 bytes exceeds");
    /* 内存池放不下：干净地失败。 */
    const wasm_host_limits_t tiny = {.pool_bytes = 1024, .stack_bytes = 2048, .linear_max_bytes = 16 * 1024, .budget = 1000};
    uint8_t *image;
    char error[128] = "";
    CHECK(!start("hello", &tiny, &image, error, sizeof error) && error[0]);
    free(image);
}

static void test_malformed_images(void)
{
    char error[128];
    uint32_t bytes;
    uint8_t *hello = load("hello", &bytes);
    /* 截断、改坏的镜像都只是加载失败。 */
    for (uint32_t cut = 0; cut < bytes; cut += 37) {
        uint8_t *image = malloc(cut ? cut : 1);
        image[0] = 0;
        memcpy(image, hello, cut);
        memset(&m, 0, sizeof m);
        CHECK(!wasm_host_start(copy_image, image, cut ? cut : 1, &LIMITS, error, sizeof error));
        free(image);
    }
    for (uint32_t index = 8; index < bytes; index += 11) {
        uint8_t *image = malloc(bytes);
        memcpy(image, hello, bytes);
        image[index] ^= 0xA5;
        memset(&m, 0, sizeof m);
        /* 改坏一个字节：可能加载失败、启动失败，也可能还能跑；都不许越界或崩溃（ASan 把关）。 */
        if (wasm_host_start(copy_image, image, bytes, &LIMITS, error, sizeof error)) {
            m.connected = true;
            wasm_host_event(WASM_EVENT_MQTT_UP, 0, 0);
            wasm_host_event(WASM_EVENT_TIMER, 0, 0);
        }
        wasm_host_stop();
        free(image);
    }
    CHECK(!wasm_host_start(NULL, NULL, 0, &LIMITS, error, sizeof error));
    /* 镜像读不出来（Flash 读失败）：干净地失败。 */
    CHECK(!wasm_host_start(unreadable, NULL, bytes, &LIMITS, error, sizeof error) && strstr(error, "image unreadable"));
    /* 镜像比内存上限还大：放不进内存池。 */
    const wasm_host_limits_t small = {.pool_bytes = 4096, .stack_bytes = 1024, .linear_max_bytes = 16 * 1024, .budget = 1000};
    static uint8_t big[8192];
    CHECK(!wasm_host_start(copy_image, big, sizeof big, &small, error, sizeof error) &&
          strstr(error, "image does not fit the memory limit"));
    free(hello);
}

/* ---------- 引脚、PWM、业务参数、设备信息、睡眠：用 io 夹具逐个接口核对 ---------- */

enum { IO_GPIO_WRITE = 1, IO_GPIO_READ, IO_PWM_CONFIG, IO_PWM_WRITE, IO_PWM_READ, IO_PARAM, IO_INFO, IO_INFO_NO_BUFFER,
       IO_SLEEP, IO_WRAPPERS, IO_GPIO_PULSE };
#define IO_TEXT INT_MAX /* 夹具发布的是文本，在 s_text 里 */
#define IO_NONE INT_MIN /* 夹具没有发布，或者出了故障 */
static char s_text[64];

static int io_send(const void *command, size_t length)
{
    const int before = m.published;
    wasm_host_event_data("io/cmd", 6, command, length);
    if (!wasm_host_event(WASM_EVENT_MQTT_MESSAGE, 6, (uint32_t)length) || m.published != before + 1) return IO_NONE;
    if (!strcmp(m.last.topic, "io/text")) {
        snprintf(s_text, sizeof s_text, "%.*s", (int)m.last.payload_length, m.last.payload);
        return IO_TEXT;
    }
    return atoi(m.last.payload);
}

static int io(uint8_t operation, uint32_t a, uint32_t b, uint32_t c)
{
    uint8_t command[13] = {operation};
    const uint32_t words[3] = {a, b, c};
    for (int word = 0; word < 3; ++word)
        for (int byte = 0; byte < 4; ++byte) command[1 + 4 * word + byte] = (uint8_t)(words[word] >> (8 * byte));
    return io_send(command, sizeof command);
}

static int io_param(const char *key)
{
    char command[40];
    const int length = snprintf(command, sizeof command, "%c%s", IO_PARAM, key);
    return io_send(command, (size_t)length);
}

static const wasm_pin_t IO_PINS[] = {{4, WASM_PIN_OUT0}, {5, WASM_PIN_OUT1}, {6, WASM_PIN_IN_PULLUP}, {7, WASM_PIN_PWM},
                                     {8, WASM_PIN_PWM}};

static void test_io(void)
{
    /* 夹具比样例大（2.8 KB），快速解释器还要再占一份预编译的代码：内存池给到 32 KiB，两种解释器都放得下。 */
    wasm_host_limits_t limits = LIMITS;
    limits.pool_bytes = 32 * 1024;
    limits.pins = IO_PINS;
    limits.pin_count = sizeof IO_PINS / sizeof IO_PINS[0];
    const wasm_host_limits_t no_pins = {.pool_bytes = limits.pool_bytes, .stack_bytes = limits.stack_bytes,
                                        .linear_max_bytes = limits.linear_max_bytes, .budget = limits.budget};
    uint8_t *image;
    char error[128] = "";
    CHECK(start("io", &limits, &image, error, sizeof error));
    if (error[0]) fprintf(stderr, "io start: %s\n", error);
    usage("io");
    /* 夹具不设遗嘱：空指针、长度 0 的参数原样通过。 */
    CHECK(m.connect_wanted && !m.will_set);
    m.connected = true;

    /* 输出脚：能写，也能读回。 */
    CHECK(io(IO_GPIO_WRITE, 4, 1, 0) == 0 && m.levels[4] == 1 && io(IO_GPIO_READ, 4, 0, 0) == 1);
    CHECK(io(IO_GPIO_WRITE, 4, 0, 0) == 0 && m.levels[4] == 0 && io(IO_GPIO_READ, 4, 0, 0) == 0);
    CHECK(io(IO_GPIO_WRITE, 5, 7, 0) == 0 && m.levels[5] == 1); /* 非 0 都是高电平 */
    /* 输入脚：只能读。 */
    m.levels[6] = 1;
    CHECK(io(IO_GPIO_READ, 6, 0, 0) == 1 && io(IO_GPIO_WRITE, 6, 0, 0) == WASM_E_DENIED && m.levels[6] == 1);
    /* 没有声明的引脚、PWM 引脚、不存在的引脚号：一律碰不到，平台不会被调用。 */
    const int writes = m.gpio_writes;
    CHECK(io(IO_GPIO_WRITE, 9, 1, 0) == WASM_E_DENIED && io(IO_GPIO_READ, 9, 0, 0) == WASM_E_DENIED);
    CHECK(io(IO_GPIO_WRITE, 7, 1, 0) == WASM_E_DENIED && io(IO_GPIO_READ, 7, 0, 0) == WASM_E_DENIED);
    CHECK(io(IO_GPIO_WRITE, 260, 1, 0) == WASM_E_DENIED && io(IO_GPIO_WRITE, 0xFFFFFFFFu, 1, 0) == WASM_E_DENIED);
    CHECK(io(IO_GPIO_READ, 0x80000004u, 0, 0) == WASM_E_DENIED && m.gpio_writes == writes);

    /* 有时限的输出：只有输出脚可以；时限 1 毫秒到 10 分钟。 */
    CHECK(io(IO_GPIO_PULSE, 4, 1, 2000) == 0 && m.levels[4] == 1 && m.pulse_ms[4] == 2000);
    CHECK(io(IO_GPIO_PULSE, 5, 0, 600000) == 0 && m.levels[5] == 0 && m.pulse_ms[5] == 600000);
    const int pulses = m.gpio_writes;
    CHECK(io(IO_GPIO_PULSE, 4, 1, 0) == WASM_E_INVALID && io(IO_GPIO_PULSE, 4, 1, 600001) == WASM_E_INVALID);
    CHECK(io(IO_GPIO_PULSE, 6, 1, 100) == WASM_E_DENIED && io(IO_GPIO_PULSE, 7, 1, 100) == WASM_E_DENIED);
    CHECK(io(IO_GPIO_PULSE, 9, 1, 100) == WASM_E_DENIED && m.gpio_writes == pulses && m.pulse_ms[4] == 2000);
    /* 到期事件带着引脚号送到应用。 */
    const int before_expiry = m.published;
    CHECK(wasm_host_event(WASM_EVENT_PIN_EXPIRED, 4, 0) && m.published == before_expiry + 1);
    CHECK(!strcmp(m.last.topic, "io/expired") && m.last.payload_length == 1 && m.last.payload[0] == '4');

    /* PWM：先配置再写；占空比不能超出分辨率。 */
    CHECK(io(IO_PWM_WRITE, 7, 100, 0) == WASM_E_NOT_READY && io(IO_PWM_READ, 7, 0, 0) == WASM_E_NOT_READY);
    CHECK(io(IO_PWM_CONFIG, 7, 50, 14) == 0 && m.pwm[7].hz == 50 && m.pwm[7].bits == 14 && m.pwm[7].duty == 0);
    CHECK(io(IO_PWM_WRITE, 7, 819, 0) == 0 && m.pwm[7].duty == 819 && io(IO_PWM_READ, 7, 0, 0) == 819);
    CHECK(io(IO_PWM_WRITE, 7, 16383, 0) == 0 && io(IO_PWM_WRITE, 7, 16384, 0) == WASM_E_INVALID && m.pwm[7].duty == 16383);
    /* 频率和分辨率的范围。 */
    const int configs = m.pwm_configs;
    CHECK(io(IO_PWM_CONFIG, 7, 0, 14) == WASM_E_INVALID && io(IO_PWM_CONFIG, 7, 40001, 14) == WASM_E_INVALID);
    CHECK(io(IO_PWM_CONFIG, 7, 50, 0) == WASM_E_INVALID && io(IO_PWM_CONFIG, 7, 50, 15) == WASM_E_INVALID);
    /* 不是 PWM 引脚的、没有声明的。 */
    CHECK(io(IO_PWM_CONFIG, 4, 50, 14) == WASM_E_DENIED && io(IO_PWM_WRITE, 4, 1, 0) == WASM_E_DENIED);
    CHECK(io(IO_PWM_READ, 6, 0, 0) == WASM_E_DENIED && io(IO_PWM_CONFIG, 9, 50, 14) == WASM_E_DENIED);
    CHECK(m.pwm_configs == configs && m.pwm[7].duty == 16383);
    /* 两个 PWM 引脚各管各的。 */
    CHECK(io(IO_PWM_WRITE, 8, 1, 0) == WASM_E_NOT_READY);
    CHECK(io(IO_PWM_CONFIG, 8, 40000, 10) == 0 && io(IO_PWM_WRITE, 8, 1023, 0) == 0);
    CHECK(io(IO_PWM_WRITE, 8, 1024, 0) == WASM_E_INVALID && m.pwm[8].duty == 1023 && m.pwm[7].duty == 16383);
    /* 平台配不出来：引脚回到未配置。 */
    CHECK(io(IO_PWM_CONFIG, 8, 39999, 10) == WASM_E_INVALID && io(IO_PWM_WRITE, 8, 1, 0) == WASM_E_NOT_READY);

    /* 业务参数：键是最多 15 个字符的小写字母、数字、下划线。 */
    CHECK(io_param("hxh_site") == IO_TEXT && !strcmp(s_text, "lab"));
    CHECK(io_param("missing") == WASM_E_NOT_FOUND && io_param("abcdefghijklmno") == WASM_E_NOT_FOUND);
    CHECK(io_param("long_value") == WASM_E_TOO_LARGE); /* 值比应用的缓冲长 */
    CHECK(io_param("") == WASM_E_INVALID && io_param("Site") == WASM_E_INVALID && io_param("a-b") == WASM_E_INVALID);
    CHECK(io_param("abcdefghijklmnop") == WASM_E_INVALID);
    /* 设备信息。 */
    CHECK(io(IO_INFO, 0, 0, 0) == IO_TEXT && !strcmp(s_text, DEVICE_ID));
    CHECK(io(IO_INFO, 1, 0, 0) == IO_TEXT && !strcmp(s_text, "esp32c3"));
    CHECK(io(IO_INFO_NO_BUFFER, 2, 0, 0) == WASM_E_INVALID && io(IO_INFO_NO_BUFFER, 0, 0, 0) == WASM_E_TOO_LARGE);

    /* 睡眠：一次调用里累计最多 1000 毫秒，超出的那次不睡；下一次调用重新计。 */
    m.sleeps = 0;
    CHECK(io(IO_SLEEP, 300, 5, 0) == WASM_E_TOO_LARGE && m.sleeps == 3);
    CHECK(io(IO_SLEEP, 1000, 1, 0) == 0 && io(IO_SLEEP, 1001, 1, 0) == WASM_E_TOO_LARGE && m.sleeps == 4);
    CHECK(io(IO_SLEEP, 250, 4, 0) == 0 && m.sleeps == 8);

    /* SDK 的封装：返回出错的步数。 */
    CHECK(io(IO_WRAPPERS, 0, 0, 0) == 0 && m.pwm[7].duty == 1228);
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    CHECK(stats.faults == 0);
    stop(image);
    CHECK(m.releases == 1);

    /* 清单没有声明引脚的应用：一个引脚也碰不到。 */
    CHECK(start("io", &no_pins, &image, error, sizeof error));
    m.connected = true;
    CHECK(io(IO_GPIO_WRITE, 4, 1, 0) == WASM_E_DENIED && io(IO_GPIO_READ, 4, 0, 0) == WASM_E_DENIED);
    CHECK(io(IO_PWM_CONFIG, 7, 50, 14) == WASM_E_DENIED && m.gpio_writes == 0 && m.pwm_configs == 0);
    CHECK(io(IO_WRAPPERS, 0, 0, 0) > 0);
    stop(image);

    /* 引脚表不合法：不启动。 */
    limits.pin_count = WASM_PACKAGE_PINS + 1;
    CHECK(!start("io", &limits, &image, error, sizeof error) && strstr(error, "invalid arguments"));
    free(image);
    limits.pin_count = 1;
    limits.pins = NULL;
    CHECK(!start("io", &limits, &image, error, sizeof error) && strstr(error, "invalid arguments"));
    free(image);
}

int main(int argc, char **argv)
{
    if (argc < 2) return fprintf(stderr, "usage: %s <tests/apps> [label]\n", argv[0]), 2;
    s_apps = argv[1];
    test_hello();
    test_io();
    test_restart_does_not_leak();
    test_faults();
    test_trap_keeps_other_events();
    test_start_failures();
    test_malformed_images();
    printf("wasm host tests (%s): %d checks, %d failures\n", argc > 2 ? argv[2] : "-", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
