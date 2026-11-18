// SPDX-License-Identifier: Apache-2.0
#include "wasm_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_export.h"
#include "wasm_port.h"

#define QOS_MAX 1
#define KEEPALIVE_MIN_S 10
#define KEEPALIVE_MAX_S 3600

static struct {
    uint8_t *pool;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t env;
    wasm_function_inst_t app_event;
    uint32_t budget, linear_bytes, calls, faults;
    bool runtime, running;
    char fault[96];
    /* 当前 MQTT 消息事件的数据，只在那一次 app_event 期间有效 */
    const char *topic;
    const void *payload;
    size_t topic_length, payload_length;
} s;

/* ---------- 宿主接口 v1（导入模块 base） ----------
 * 指针参数都由 WAMR 按签名里的 *~ 核对过范围；应用传空指针时这里拿到的是 NULL，长度必须为 0。 */

static void n_log(wasm_exec_env_t env, int32_t level, const char *text, uint32_t length)
{
    (void)env;
    if (text && length) wasm_port_log(level, text, length);
}

static int64_t n_uptime_ms(wasm_exec_env_t env)
{
    (void)env;
    return (int64_t)wasm_port_uptime_ms();
}

static int64_t n_unix_ms(wasm_exec_env_t env)
{
    (void)env;
    return (int64_t)wasm_port_unix_ms();
}

static int32_t n_timer_set(wasm_exec_env_t env, uint32_t id, uint32_t delay_ms, uint32_t period_ms)
{
    (void)env;
    if (id >= WASM_TIMERS || delay_ms == 0) return WASM_E_INVALID;
    return wasm_port_timer_set(id, delay_ms, period_ms);
}

static int32_t n_timer_cancel(wasm_exec_env_t env, uint32_t id)
{
    (void)env;
    return id < WASM_TIMERS ? wasm_port_timer_cancel(id) : WASM_E_INVALID;
}

static int32_t n_sleep_ms(wasm_exec_env_t env, uint32_t ms)
{
    (void)env;
    if (ms > WASM_SLEEP_MAX_MS) return WASM_E_TOO_LARGE;
    wasm_port_sleep_ms(ms);
    return WASM_OK;
}

static int32_t n_mqtt_connect(wasm_exec_env_t env, const char *will_topic, uint32_t will_topic_length,
                              const void *will_payload, uint32_t will_payload_length, int32_t will_qos,
                              int32_t will_retain, uint32_t keepalive_s)
{
    (void)env;
    if (will_topic_length > WASM_TOPIC_BYTES || will_payload_length > WASM_PAYLOAD_BYTES) return WASM_E_TOO_LARGE;
    if (will_qos < 0 || will_qos > QOS_MAX || keepalive_s < KEEPALIVE_MIN_S || keepalive_s > KEEPALIVE_MAX_S)
        return WASM_E_INVALID;
    if ((will_topic_length && !will_topic) || (will_payload_length && !will_payload)) return WASM_E_INVALID;
    /* 主题长度为 0 表示不设遗嘱。 */
    const wasm_port_will_t will = {.topic = will_topic_length ? will_topic : NULL, .topic_length = will_topic_length,
                                   .payload = will_payload, .payload_length = will_payload_length, .qos = will_qos,
                                   .retain = will_retain != 0};
    return wasm_port_mqtt_connect(&will, keepalive_s);
}

static int32_t n_mqtt_disconnect(wasm_exec_env_t env)
{
    (void)env;
    return wasm_port_mqtt_disconnect();
}

static int32_t n_mqtt_subscribe(wasm_exec_env_t env, const char *filter, uint32_t length, int32_t qos)
{
    (void)env;
    if (!filter || length == 0 || qos < 0 || qos > QOS_MAX) return WASM_E_INVALID;
    if (length > WASM_TOPIC_BYTES) return WASM_E_TOO_LARGE;
    return wasm_port_mqtt_subscribe(filter, length, qos);
}

static int32_t n_mqtt_publish(wasm_exec_env_t env, const char *topic, uint32_t topic_length, const void *payload,
                              uint32_t payload_length, int32_t qos, int32_t retain)
{
    (void)env;
    if (!topic || topic_length == 0 || qos < 0 || qos > QOS_MAX || (payload_length && !payload)) return WASM_E_INVALID;
    if (topic_length > WASM_TOPIC_BYTES || payload_length > WASM_PAYLOAD_BYTES) return WASM_E_TOO_LARGE;
    return wasm_port_mqtt_publish(topic, topic_length, payload, payload_length, qos, retain != 0);
}

static int32_t copy_out(void *out, uint32_t capacity, const void *data, size_t length)
{
    if (!data) return WASM_E_NOT_READY; /* 当前事件不带数据 */
    if (length > capacity) return WASM_E_TOO_LARGE;
    if (length) memcpy(out, data, length);
    return (int32_t)length;
}

static int32_t n_event_topic(wasm_exec_env_t env, char *out, uint32_t capacity)
{
    (void)env;
    return copy_out(out, capacity, s.topic, s.topic_length);
}

static int32_t n_event_payload(wasm_exec_env_t env, void *out, uint32_t capacity)
{
    (void)env;
    return copy_out(out, capacity, s.topic ? s.payload : NULL, s.payload_length);
}

/* 签名：i 是 i32，I 是 i64，* 是应用内存里的指针，~ 是它前面那个缓冲的长度。WAMR 会就地给这张表排序，所以不是 const。 */
static NativeSymbol s_natives[] = {
    {"log", n_log, "(i*~)", NULL},
    {"uptime_ms", n_uptime_ms, "()I", NULL},
    {"unix_ms", n_unix_ms, "()I", NULL},
    {"timer_set", n_timer_set, "(iii)i", NULL},
    {"timer_cancel", n_timer_cancel, "(i)i", NULL},
    {"sleep_ms", n_sleep_ms, "(i)i", NULL},
    {"mqtt_connect", n_mqtt_connect, "(*~*~iii)i", NULL},
    {"mqtt_disconnect", n_mqtt_disconnect, "()i", NULL},
    {"mqtt_subscribe", n_mqtt_subscribe, "(*~i)i", NULL},
    {"mqtt_publish", n_mqtt_publish, "(*~*~ii)i", NULL},
    {"event_topic", n_event_topic, "(*~)i", NULL},
    {"event_payload", n_event_payload, "(*~)i", NULL},
};

/* ---------- 生命周期 ---------- */

static bool invoke(wasm_function_inst_t function, uint32_t argc, uint32_t *argv)
{
    wasm_runtime_set_instruction_count_limit(s.env, (int)s.budget);
    ++s.calls;
    if (wasm_runtime_call_wasm(s.env, function, argc, argv)) return true;
    const char *exception = wasm_runtime_get_exception(s.instance);
    /* WAMR 的文本形如 "Exception: unreachable"：去掉前缀，留下原因。 */
    if (exception && !strncmp(exception, "Exception: ", 11)) exception += 11;
    snprintf(s.fault, sizeof s.fault, "%s", exception ? exception : "unknown");
    ++s.faults;
    s.running = false;
    return false;
}

static bool failed(char *error, size_t error_size, const char *stage, const char *detail)
{
    if (error && error_size) snprintf(error, error_size, "%s: %s", stage, detail);
    wasm_host_stop();
    return false;
}

bool wasm_host_start(uint8_t *image, uint32_t image_bytes, const wasm_host_limits_t *limits, char *error,
                     size_t error_size)
{
    char detail[96] = "";
    wasm_host_stop();
    if (!image || !image_bytes || !limits || !limits->pool_bytes || !limits->stack_bytes || !limits->budget)
        return failed(error, error_size, "start", "invalid arguments");
    s.budget = limits->budget;
    if (!(s.pool = malloc(limits->pool_bytes))) return failed(error, error_size, "start", "no memory for the pool");

    RuntimeInitArgs init = {.mem_alloc_type = Alloc_With_Pool};
    init.mem_alloc_option.pool.heap_buf = s.pool;
    init.mem_alloc_option.pool.heap_size = limits->pool_bytes;
    init.native_module_name = "base";
    init.native_symbols = s_natives;
    init.n_native_symbols = sizeof s_natives / sizeof s_natives[0];
    if (!wasm_runtime_full_init(&init)) return failed(error, error_size, "start", "runtime init failed");
    s.runtime = true;

    if (!(s.module = wasm_runtime_load(image, image_bytes, detail, sizeof detail)))
        return failed(error, error_size, "load", detail);
    /* 应用用到的宿主接口这个固件都要有：缺一个就不装，而不是等运行到那里才陷阱。 */
    const int32_t imports = wasm_runtime_get_import_count(s.module);
    for (int32_t index = 0; index < imports; ++index) {
        wasm_import_t import = {0};
        wasm_runtime_get_import_type(s.module, index, &import);
        if (import.kind == WASM_IMPORT_EXPORT_KIND_FUNC && import.linked) continue;
        snprintf(detail, sizeof detail, "%s.%s is not provided by this firmware", import.module_name ? import.module_name : "?",
                 import.name ? import.name : "?");
        return failed(error, error_size, "load", detail);
    }
    if (!(s.instance = wasm_runtime_instantiate(s.module, limits->stack_bytes, 0, detail, sizeof detail)))
        return failed(error, error_size, "instantiate", detail);
    const wasm_memory_inst_t memory = wasm_runtime_get_default_memory(s.instance);
    const uint64_t linear = memory ? wasm_memory_get_cur_page_count(memory) * wasm_memory_get_bytes_per_page(memory) : 0;
    if (linear > limits->linear_max_bytes) {
        /* 没有收缩：模块含 memory.grow，或没有导出 __heap_base、__data_end。 */
        snprintf(detail, sizeof detail, "linear memory %llu bytes exceeds %u", (unsigned long long)linear,
                 (unsigned)limits->linear_max_bytes);
        return failed(error, error_size, "instantiate", detail);
    }
    s.linear_bytes = (uint32_t)linear;
    if (!(s.env = wasm_runtime_create_exec_env(s.instance, limits->stack_bytes)))
        return failed(error, error_size, "instantiate", "no memory for the execution environment");
    const wasm_function_inst_t app_init = wasm_runtime_lookup_function(s.instance, "app_init");
    s.app_event = wasm_runtime_lookup_function(s.instance, "app_event");
    if (!app_init || !s.app_event) return failed(error, error_size, "instantiate", "app_init or app_event is not exported");

    uint32_t argv[1] = {0};
    if (!invoke(app_init, 0, argv)) {
        snprintf(detail, sizeof detail, "%s", s.fault);
        return failed(error, error_size, "app_init", detail);
    }
    if (argv[0] != 0) {
        snprintf(detail, sizeof detail, "returned %d", (int)argv[0]);
        return failed(error, error_size, "app_init", detail);
    }
    s.running = true;
    return true;
}

bool wasm_host_event(wasm_event_kind_t kind, uint32_t a, uint32_t b)
{
    if (!s.running) return false;
    uint32_t argv[3] = {(uint32_t)kind, a, b};
    const bool ok = invoke(s.app_event, 3, argv);
    s.topic = NULL;
    s.payload = NULL;
    s.topic_length = s.payload_length = 0;
    return ok;
}

void wasm_host_event_data(const char *topic, size_t topic_length, const void *payload, size_t payload_length)
{
    s.topic = topic;
    s.topic_length = topic_length;
    s.payload = payload;
    s.payload_length = payload_length;
}

const char *wasm_host_fault(void)
{
    return s.fault;
}

bool wasm_host_running(void)
{
    return s.running;
}

void wasm_host_stats(wasm_host_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s.runtime) return;
    mem_alloc_info_t pool = {0};
    if (wasm_runtime_get_mem_alloc_info(&pool)) {
        out->pool_used = pool.total_size - pool.total_free_size;
        out->pool_peak = pool.highmark_size;
    }
    out->linear_bytes = s.linear_bytes;
    out->calls = s.calls;
    out->faults = s.faults;
}

void wasm_host_stop(void)
{
    /* 先让平台停掉定时器和连接：之后不会再有事件指向这个实例，输出也回到安全状态。 */
    if (s.runtime) wasm_port_release();
    if (s.env) wasm_runtime_destroy_exec_env(s.env);
    if (s.instance) wasm_runtime_deinstantiate(s.instance);
    if (s.module) wasm_runtime_unload(s.module);
    if (s.runtime) wasm_runtime_destroy();
    free(s.pool);
    char fault[sizeof s.fault];
    memcpy(fault, s.fault, sizeof fault); /* 停止后仍可读到最近一次故障的原因 */
    memset(&s, 0, sizeof s);
    memcpy(s.fault, fault, sizeof fault);
}
