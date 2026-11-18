// SPDX-License-Identifier: Apache-2.0
// 宿主接口背后的平台服务。设备上由 ESP-IDF 实现（port/wasm_port_idf.c），host 测试里由桩实现；
// 宿主核心（wasm_host.c）只经这里接触平台，所以同一份核心代码在设备和本机上都能跑。
//
// 约定：字符串都带长度、不以 NUL 结尾；返回值 0 或正数表示成功，负数是 wasm_error_t。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 宿主接口的错误码：应用拿到的就是这些数。只增不改。 */
typedef enum {
    WASM_OK = 0,
    WASM_E_INVALID = -1,   /* 参数不合法 */
    WASM_E_DENIED = -2,    /* 清单没有声明，或属于系统 */
    WASM_E_NOT_READY = -3, /* 还没连上、还没对时 */
    WASM_E_NO_MEMORY = -4, /* 内存或队列不够 */
    WASM_E_TOO_LARGE = -5, /* 超出长度上限或缓冲太小 */
    WASM_E_NOT_FOUND = -6, /* 没有这个键 */
} wasm_error_t;

#define WASM_TIMERS 4
#define WASM_TOPIC_BYTES 128    /* 主题的最大长度 */
#define WASM_PAYLOAD_BYTES 1024 /* 单条消息载荷的最大长度 */
#define WASM_SLEEP_MAX_MS 1000

typedef struct {
    const char *topic; /* 空：不设遗嘱 */
    size_t topic_length;
    const void *payload;
    size_t payload_length;
    int qos;
    bool retain;
} wasm_port_will_t;

void wasm_port_log(int level, const char *text, size_t length);
uint64_t wasm_port_uptime_ms(void);
uint64_t wasm_port_unix_ms(void); /* 0：还没对时 */
int wasm_port_timer_set(unsigned id, uint32_t delay_ms, uint32_t period_ms);
int wasm_port_timer_cancel(unsigned id);
void wasm_port_sleep_ms(uint32_t ms);
/* 业务 MQTT：地址和账户由系统保存，应用拿不到。connect 只登记意图和遗嘱，连接由平台在合适的时机建立并自动重连；
 * 订阅由平台记住，每次连上后重新订阅。 */
int wasm_port_mqtt_connect(const wasm_port_will_t *will, unsigned keepalive_s);
int wasm_port_mqtt_disconnect(void);
int wasm_port_mqtt_subscribe(const char *filter, size_t length, int qos);
int wasm_port_mqtt_publish(const char *topic, size_t topic_length, const void *payload, size_t payload_length, int qos,
                           bool retain);
/* 应用停止（陷阱、替换、回退）：取消定时器、断开连接、把输出置回安全状态。 */
void wasm_port_release(void);
