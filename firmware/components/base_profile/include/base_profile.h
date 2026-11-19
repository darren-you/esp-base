// SPDX-License-Identifier: Apache-2.0
// 设备 Profile 的唯一来源：芯片、MAC、IDF 版本在启动时采集一次；名称、Group、系统 MQTT 地址等设置由控制任务写入；
// 堆、系统任务栈、Wi-Fi 与 IPv4／IPv6 由采样任务按请求异步采样，业务层的部分（名称、状态、连接、业务任务栈）
// 在同一次采样里向业务运行时读取，采完回调通知。状态文档和遗嘱都从这里读取，不在别处另行采集。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_protocol.h"
#include "esp_err.h"

/* 一次采样完成；在采样任务里调用，只应转发事件。 */
typedef void (*base_profile_ready_cb_t)(void);

/* 采集启动时就确定的信息并启动采样任务。之后才能调用下面的函数。 */
esp_err_t base_profile_start(const char *device_id, const char *boot_id, const char *reset,
                             const base_fw_t *fw, base_profile_ready_cb_t ready);

/* 控制任务持有的设置。字符串为空表示没有；name 为空时上报默认名称。 */
typedef struct {
    const char *name;
    const char *group;
    const char *mqtt_uri;
    uint32_t config_revision;
    const char *config_error;
    uint32_t wakeups;
} base_profile_settings_t;
void base_profile_set(const base_profile_settings_t *settings);

/* 公网出口地址由控制任务写入（取到、或网络断开时清掉）；NULL 或空串表示还没取到。 */
void base_profile_set_public_ip(const char *ip);

/* 请求一次异步采样；正在采样时合并为一次。完成后调用 base_profile_start 给出的回调。 */
void base_profile_request(void);

/* 唯一的读取方式：取最近一次的完整快照。尚未启动时返回 false。 */
bool base_profile_get(base_profile_t *out);

/* 按快照编码状态文档与遗嘱；返回写入长度，失败返回 -1。 */
int base_profile_status(char *out, size_t size);
int base_profile_will(char *out, size_t size);
