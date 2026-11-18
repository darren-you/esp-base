// SPDX-License-Identifier: Apache-2.0
// ESP Base 系统层的入口。系统层负责联网、系统 MQTT、状态文档、命令、远程配置和 OTA；
// 业务固件在自己的 app_main 里调用 base_system_run 并交出业务描述，业务层只依赖 base_runtime.h。
#pragma once
#include "base_runtime.h"

/* 运行系统，不返回。business 为空时固件没有业务层。 */
void base_system_run(const base_business_t *business);
