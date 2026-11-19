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
    uint8_t *pool, *image;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t env;
    wasm_function_inst_t app_event;
    uint32_t budget, linear_bytes, calls, faults;
    uint32_t sleep_left_ms; /* 这一次调用里还能睡多久 */
    const wasm_pin_t *pins;
    uint8_t pin_count;
    uint8_t pwm_bits[WASM_PACKAGE_PINS]; /* 与 pins 对应：配置过的 PWM 分辨率，0 表示还没配置 */
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
    /* 应用任务睡着时事件和部署都在排队：一次调用里累计能睡的时间有上限，死循环着睡也拖不住宿主。 */
    if (ms > s.sleep_left_ms) return WASM_E_TOO_LARGE;
    s.sleep_left_ms -= ms;
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

static int32_t n_param(wasm_exec_env_t env, const char *key, uint32_t key_length, char *out, uint32_t capacity)
{
    (void)env;
    char name[WASM_PARAM_KEY_BYTES + 1];
    if (!key || key_length == 0 || key_length > WASM_PARAM_KEY_BYTES || (capacity && !out)) return WASM_E_INVALID;
    for (uint32_t index = 0; index < key_length; ++index) {
        const char c = key[index];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return WASM_E_INVALID;
        name[index] = c;
    }
    name[key_length] = '\0';
    return wasm_port_param(name, out, capacity);
}

static int32_t n_info(wasm_exec_env_t env, int32_t what, char *out, uint32_t capacity)
{
    (void)env;
    if ((what != WASM_INFO_DEVICE_ID && what != WASM_INFO_CHIP) || (capacity && !out)) return WASM_E_INVALID;
    return wasm_port_info((wasm_info_t)what, out, capacity);
}

/* 引脚必须在清单里按相应的用途声明过：没有声明的一律拒绝，应用碰不到别的引脚。 */
static int pin_index(uint32_t pin, bool want_output, bool want_pwm)
{
    for (int index = 0; index < s.pin_count; ++index) {
        if (s.pins[index].pin != pin) continue;
        const wasm_pin_mode_t mode = (wasm_pin_mode_t)s.pins[index].mode;
        if (want_pwm) return mode == WASM_PIN_PWM ? index : -1;
        if (want_output) return mode == WASM_PIN_OUT0 || mode == WASM_PIN_OUT1 ? index : -1;
        return mode == WASM_PIN_PWM ? -1 : index; /* 读电平：输入脚和普通输出脚都可以 */
    }
    return -1;
}

static int32_t n_gpio_write(wasm_exec_env_t env, uint32_t pin, int32_t level)
{
    (void)env;
    return pin_index(pin, true, false) < 0 ? WASM_E_DENIED : wasm_port_gpio_write(pin, level != 0);
}

static int32_t n_gpio_pulse(wasm_exec_env_t env, uint32_t pin, int32_t level, uint32_t ms)
{
    (void)env;
    if (pin_index(pin, true, false) < 0) return WASM_E_DENIED;
    if (ms == 0 || ms > WASM_PULSE_MAX_MS) return WASM_E_INVALID;
    return wasm_port_gpio_pulse(pin, level != 0, ms);
}

static int32_t n_gpio_read(wasm_exec_env_t env, uint32_t pin)
{
    (void)env;
    return pin_index(pin, false, false) < 0 ? WASM_E_DENIED : wasm_port_gpio_read(pin);
}

static int32_t n_pwm_config(wasm_exec_env_t env, uint32_t pin, uint32_t frequency_hz, uint32_t resolution_bits)
{
    (void)env;
    const int index = pin_index(pin, false, true);
    if (index < 0) return WASM_E_DENIED;
    if (frequency_hz == 0 || frequency_hz > WASM_PWM_HZ_MAX || resolution_bits == 0 || resolution_bits > WASM_PWM_BITS_MAX)
        return WASM_E_INVALID;
    const int result = wasm_port_pwm_config(pin, frequency_hz, resolution_bits);
    s.pwm_bits[index] = result == WASM_OK ? (uint8_t)resolution_bits : 0; /* 没配成：回到未配置 */
    return result;
}

static int32_t n_pwm_write(wasm_exec_env_t env, uint32_t pin, uint32_t duty)
{
    (void)env;
    const int index = pin_index(pin, false, true);
    if (index < 0) return WASM_E_DENIED;
    if (!s.pwm_bits[index]) return WASM_E_NOT_READY;         /* 还没配置 */
    if (duty >> s.pwm_bits[index]) return WASM_E_INVALID;     /* 超出分辨率 */
    return wasm_port_pwm_write(pin, duty);
}

static int32_t n_pwm_read(wasm_exec_env_t env, uint32_t pin)
{
    (void)env;
    const int index = pin_index(pin, false, true);
    if (index < 0) return WASM_E_DENIED;
    return s.pwm_bits[index] ? wasm_port_pwm_read(pin) : WASM_E_NOT_READY;
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
    {"param", n_param, "(*~*~)i", NULL},
    {"info", n_info, "(i*~)i", NULL},
    {"gpio_write", n_gpio_write, "(ii)i", NULL},
    {"gpio_read", n_gpio_read, "(i)i", NULL},
    {"gpio_pulse", n_gpio_pulse, "(iii)i", NULL},
    {"pwm_config", n_pwm_config, "(iii)i", NULL},
    {"pwm_write", n_pwm_write, "(ii)i", NULL},
    {"pwm_read", n_pwm_read, "(i)i", NULL},
};

/* ---------- 生命周期 ---------- */

static bool invoke(wasm_function_inst_t function, uint32_t argc, uint32_t *argv)
{
    wasm_runtime_set_instruction_count_limit(s.env, (int)s.budget);
    s.sleep_left_ms = WASM_SLEEP_MAX_MS;
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

bool wasm_host_start(wasm_host_read_t read, void *context, uint32_t image_bytes, const wasm_host_limits_t *limits,
                     char *error, size_t error_size)
{
    char detail[96] = "";
    wasm_host_stop();
    if (!read || !image_bytes || !limits || !limits->pool_bytes || !limits->stack_bytes || !limits->budget)
        return failed(error, error_size, "start", "invalid arguments");
    if (limits->pin_count > WASM_PACKAGE_PINS || (limits->pin_count && !limits->pins))
        return failed(error, error_size, "start", "invalid arguments");
    s.budget = limits->budget;
    s.pins = limits->pins;
    s.pin_count = limits->pin_count;
    if (!(s.pool = malloc(limits->pool_bytes))) return failed(error, error_size, "start", "no memory for the pool");

    RuntimeInitArgs init = {.mem_alloc_type = Alloc_With_Pool};
    init.mem_alloc_option.pool.heap_buf = s.pool;
    init.mem_alloc_option.pool.heap_size = limits->pool_bytes;
    init.native_module_name = "base";
    init.native_symbols = s_natives;
    init.n_native_symbols = sizeof s_natives / sizeof s_natives[0];
    if (!wasm_runtime_full_init(&init)) return failed(error, error_size, "start", "runtime init failed");
    s.runtime = true;

    if (!(s.image = wasm_runtime_malloc(image_bytes)))
        return failed(error, error_size, "start", "image does not fit the memory limit");
    if (!read(context, s.image, image_bytes)) return failed(error, error_size, "start", "image unreadable");
    if (!(s.module = wasm_runtime_load(s.image, image_bytes, detail, sizeof detail)))
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
    if (s.image) wasm_runtime_free(s.image);
    if (s.runtime) wasm_runtime_destroy();
    free(s.pool);
    char fault[sizeof s.fault];
    memcpy(fault, s.fault, sizeof fault); /* 停止后仍可读到最近一次故障的原因 */
    memset(&s, 0, sizeof s);
    memcpy(s.fault, fault, sizeof fault);
}
