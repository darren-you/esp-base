// SPDX-License-Identifier: Apache-2.0
// host 测试：宿主核心加载真实的应用（tests/apps/*.wasm，源码在 sdk/rust），核对宿主接口、事件分发，
// 以及每一类应用故障都只停掉应用：陷阱、死循环、栈耗尽、越界指针、缺少的宿主接口、没有收缩的线性内存。
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
    publish_t publishes[RECORDS];
    bool connect_wanted, connected;
    publish_t will;
    bool will_set;
    unsigned keepalive_s;
    int subscriptions;
    char filters[4][WASM_TOPIC_BYTES + 1];
    struct { bool active; uint32_t delay_ms, period_ms; } timers[WASM_TIMERS];
    uint64_t uptime_ms, unix_ms;
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
    ++m.published;
    return WASM_OK;
}

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

static bool start(const char *name, const wasm_host_limits_t *limits, uint8_t **image, char *error, size_t error_size)
{
    uint32_t bytes;
    memset(&m, 0, sizeof m);
    *image = load(name, &bytes);
    return wasm_host_start(*image, bytes, limits, error, error_size);
}

static void stop(uint8_t *image)
{
    wasm_host_stop();
    free(image);
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
    expect_start_failure("init_fail", &LIMITS, "app_init: returned 1");
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
        memcpy(image, hello, cut);
        memset(&m, 0, sizeof m);
        CHECK(!wasm_host_start(image, cut, &LIMITS, error, sizeof error));
        free(image);
    }
    for (uint32_t index = 8; index < bytes; index += 11) {
        uint8_t *image = malloc(bytes);
        memcpy(image, hello, bytes);
        image[index] ^= 0xA5;
        memset(&m, 0, sizeof m);
        /* 改坏一个字节：可能加载失败、启动失败，也可能还能跑；都不许越界或崩溃（ASan 把关）。 */
        if (wasm_host_start(image, bytes, &LIMITS, error, sizeof error)) {
            m.connected = true;
            wasm_host_event(WASM_EVENT_MQTT_UP, 0, 0);
            wasm_host_event(WASM_EVENT_TIMER, 0, 0);
        }
        wasm_host_stop();
        free(image);
    }
    free(hello);
    CHECK(!wasm_host_start(NULL, 0, &LIMITS, error, sizeof error));
}

int main(int argc, char **argv)
{
    if (argc < 2) return fprintf(stderr, "usage: %s <tests/apps> [label]\n", argv[0]), 2;
    s_apps = argv[1];
    test_hello();
    test_restart_does_not_leak();
    test_faults();
    test_trap_keeps_other_events();
    test_start_failures();
    test_malformed_images();
    printf("wasm host tests (%s): %d checks, %d failures\n", argc > 2 ? argv[2] : "-", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
