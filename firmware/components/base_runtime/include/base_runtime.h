// SPDX-License-Identifier: Apache-2.0
// 业务运行时：业务层唯一依赖的 esp-base 接口。
//
// esp-base 分两层。系统层负责联网、系统 MQTT、命令、远程配置、OTA 和设备 Profile；业务层只实现自己的功能。
// 业务通过这里取得设备身份、业务参数和业务 MQTT 配置，创建任务，打开自己的连接；它看不到系统的 Wi-Fi、
// 系统 MQTT 账户和 NVS，系统层的升级、回滚和管理也不经过业务代码。
//
// 这是代码上的解耦，不是内存隔离：业务与系统在同一个镜像、同一个地址空间里（C3 这类芯片没有条件做真正的隔离）。
// 运行时能做到的是：业务任务的优先级低于系统任务，业务反复把设备弄到异常重启时不再拉起业务（安全模式），
// 系统照常在线，可以远程升级修复。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqtt_client.h"

/* 业务任务的最高优先级：系统任务都在它之上，业务任务占满 CPU 时系统仍能应答命令和升级。 */
#define BASE_RUNTIME_PRIORITY_MAX 4

/* 业务固件在 app_main 里把它交给 base_system_run。 */
typedef struct {
    /* 业务名：1–23 字节的小写字母、数字和下划线；上报在状态文档的 business.name。 */
    const char *name;
    /* 启动时、联网之前调用一次：把输出置到安全状态、读业务参数、建任务。
     * 安全模式下不调用（任何业务代码都不运行），所以硬件必须在引脚保持复位状态时就是安全的。 */
    void (*init)(void);
    /* 该（重新）建立业务连接了：系统 MQTT 首次连上后调用一次（60 秒内连不上则不再等），
     * 业务 MQTT 配置经远程配置更新后再调用一次。在系统的控制任务里调用，不要阻塞。 */
    void (*connect)(void);
    /* 可选：应用部署通道。系统在自己的主题 app/desired 上收到消息时调用，payload 是原文，长度 0 表示取消；
     * 返回后 payload 失效。在系统的控制任务里调用，不要阻塞。 */
    void (*deploy)(const char *payload, size_t length);
    /* 可选：把要发布到 app/state 的内容写进 out，返回长度；没有可报的返回 0。在系统的控制任务里调用。 */
    int (*state)(char *out, size_t size);
} base_business_t;

/* app/state 的内容变了，请系统重新发布。在任意任务里调用。 */
void base_runtime_app_changed(void);
/* 系统正在下载或校验固件：这时业务不要开始自己的下载。 */
bool base_runtime_system_updating(void);

/* 这次启动之前设备是不是异常重启的（panic、各类看门狗，或业务看门狗超时），而且重启前运行的就是这个固件。
 * 系统的业务守卫按它计数；业务也可以据此判断是不是自己把设备拖垮了。 */
bool base_runtime_abnormal_restart(void);

/* 设备 UUID；业务连接的客户端 ID 不能直接用它（系统 MQTT 在用）。 */
const char *base_runtime_device_id(void);

/* 业务参数：出厂资料里 business 对象的字符串值。没有这个键时返回 ESP_ERR_NVS_NOT_FOUND，out 为空串。 */
esp_err_t base_runtime_param(const char *key, char *out, size_t size);

/* 业务 MQTT：系统代为保存、可以远程配置的一个 Broker（地址、账户、密码）。业务可以用它，
 * 也可以按自己的参数连接别的服务器。没有配置时返回 false。含密码：用完清零。 */
typedef struct {
    char uri[128];
    char username[65];
    char password[129];
} base_runtime_mqtt_t;
bool base_runtime_mqtt_config(base_runtime_mqtt_t *out);

/* 创建业务任务，记在业务名下（状态文档的 business.tasks）。priority 超过 BASE_RUNTIME_PRIORITY_MAX 时按上限。
 * watchdog_ms 非 0：任务必须每隔不超过这么久调用一次 base_runtime_feed，否则运行时重启设备并记一次业务故障。
 * 任务函数可以返回（任务随之结束）。最多 4 个。 */
esp_err_t base_runtime_task(const char *name, void (*entry)(void *argument), void *argument, uint32_t stack_bytes,
                            unsigned priority, uint32_t watchdog_ms);
/* 在带看门狗的业务任务里调用：我还活着。 */
void base_runtime_feed(void);

/* 打开一条业务 MQTT 连接，记在业务名下（状态文档的 business.links：名称、地址、是否已连上）。
 * 返回官方 esp-mqtt 的 client，还没有启动：业务先用 esp_mqtt_client_register_event 注册自己的事件，再调用
 * esp_mqtt_client_start，之后订阅、发布都用 esp-mqtt 的接口。name 是 1–15 字节的小写字母、数字和下划线。
 * 连接任务的优先级固定为 BASE_RUNTIME_PRIORITY_MAX：与系统 MQTT 同时握手时系统优先。
 * 关闭必须用 base_runtime_mqtt_close（不要自己 stop／destroy）。最多 3 条；失败返回 NULL。 */
esp_mqtt_client_handle_t base_runtime_mqtt_open(const char *name, const esp_mqtt_client_config_t *config);
void base_runtime_mqtt_close(esp_mqtt_client_handle_t client);
