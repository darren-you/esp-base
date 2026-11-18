// SPDX-License-Identifier: Apache-2.0
// ESP Base 主程序：系统 MQTT、状态文档、命令、配置试用和 OTA。业务固件在自己的 app_main 里调用
// base_app_run 并传入业务层的回调；业务层另开自己的连接，不使用系统 MQTT 的主题和账户。
#pragma once
#include "base_store.h"

typedef struct {
    /* 控制任务启动时调用一次，在联网之前：把输出脚置到安全状态、读取业务参数。
     * device_id 是设备 UUID；config 为空表示设备还没有出厂配置。 */
    void (*init)(const char *device_id, const base_config_t *config);
    /* 系统 MQTT 首次连上后调用一次（60 秒内连不上则不再等），业务 MQTT 经 config 命令更新后再调用一次。
     * 错开两条连接的 TLS 握手：同时握手会让单核芯片连续数秒占满 CPU。
     * 业务层按传入的配置（重新）建立自己的连接；uri 为空表示没有配置。在控制任务里调用，不要阻塞。 */
    void (*connect)(const base_mqtt_t *business_mqtt);
} base_business_t;

/* 运行基座，不返回。business 为空时只运行基座本身。 */
void base_app_run(const base_business_t *business);
