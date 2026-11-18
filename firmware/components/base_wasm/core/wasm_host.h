// SPDX-License-Identifier: Apache-2.0
// 宿主核心：用 WAMR 加载一个应用，注册宿主接口，把事件逐个交给应用。不依赖 ESP-IDF。
//
// 同一时刻只有一个应用；所有调用都来自同一个任务（设备上是应用任务）。应用导出 app_init 和 app_event，
// 每次调用都有指令预算：陷阱或超预算只让应用停下，由调用方决定重启还是回退。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_package.h"

/* 事件种类：app_event 的第一个参数。只增不改。 */
typedef enum {
    WASM_EVENT_TIMER = 1,        /* a：定时器编号 */
    WASM_EVENT_MQTT_UP = 2,      /* 业务 MQTT 连上了 */
    WASM_EVENT_MQTT_DOWN = 3,    /* 业务 MQTT 断开了 */
    WASM_EVENT_MQTT_MESSAGE = 4, /* a：主题长度，b：载荷长度；数据用 event_topic、event_payload 读取 */
} wasm_event_kind_t;

typedef struct {
    uint32_t pool_bytes;        /* 运行时内存池：镜像、模块、实例和执行环境都在里面，也就是应用的内存上限 */
    uint32_t stack_bytes;       /* 应用的操作数栈 */
    uint32_t linear_max_bytes;  /* 线性内存上限：超过说明模块没有按要求链接（没有收缩） */
    uint32_t budget;            /* 每次调用的指令预算 */
    const wasm_pin_t *pins;     /* 清单里声明的引脚：应用只能按声明的用途使用它们；在应用停止之前保持有效 */
    uint8_t pin_count;
} wasm_host_limits_t;

typedef struct {
    uint32_t pool_used, pool_peak; /* 字节 */
    uint32_t linear_bytes;
    uint32_t calls, faults;
} wasm_host_stats_t;

/* 把应用镜像（wasm 模块）的 bytes 字节读进 out。 */
typedef bool (*wasm_host_read_t)(void *context, uint8_t *out, uint32_t bytes);

/* 启动应用：初始化运行时、读入镜像、加载、实例化并调用 app_init。镜像读进内存池里的缓冲（加载器会原地改写
 * 字节码，所以不能直接用 Flash 里的那份），计入应用的内存上限。失败时 error 里是原因，运行时已经清理干净。 */
bool wasm_host_start(wasm_host_read_t read, void *context, uint32_t image_bytes, const wasm_host_limits_t *limits,
                     char *error, size_t error_size);
/* 把一个事件交给应用。返回假表示应用出了故障（陷阱、超预算），原因见 wasm_host_fault；之后只能 stop。 */
bool wasm_host_event(wasm_event_kind_t kind, uint32_t a, uint32_t b);
/* MQTT 消息事件期间可读的数据：调用 wasm_host_event(WASM_EVENT_MQTT_MESSAGE, …) 之前设置，返回后失效。 */
void wasm_host_event_data(const char *topic, size_t topic_length, const void *payload, size_t payload_length);
const char *wasm_host_fault(void);
bool wasm_host_running(void);
void wasm_host_stats(wasm_host_stats_t *out);
/* 停止应用并释放运行时；可以重复调用。 */
void wasm_host_stop(void);
