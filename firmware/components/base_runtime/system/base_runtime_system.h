// SPDX-License-Identifier: Apache-2.0
// 业务运行时留给系统层的接口：启动业务、传递业务 MQTT 配置、读取业务在 Profile 里的部分。业务层看不到这个文件。
#pragma once
#include "base_protocol.h"
#include "base_runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 业务的连接或状态变了，状态文档需要重新发布。在任意任务里调用，只应转发事件。 */
typedef void (*base_runtime_changed_cb_t)(void);

/* 上一次是不是运行时因业务看门狗超时而重启了设备。启动时调用一次，读后清除。 */
bool base_runtime_take_fault(void);

/* 登记业务并启动。business 为空表示固件没有业务层；mqtt 是已保存的业务 MQTT 配置。
 * enabled 为假时进入安全模式：不调用任何业务代码，状态文档里 business.state 为 disabled、error 为给出的原因。
 * enabled 为真时调用 business->init。 */
void base_runtime_boot(const base_business_t *business, const char *device_id, const base_mqtt_t *mqtt, bool enabled,
                       const char *error, base_runtime_changed_cb_t changed);
/* 有业务层且在运行。 */
bool base_runtime_active(void);
/* 业务 MQTT 配置经远程配置更新后由系统写入。 */
void base_runtime_set_mqtt(const base_mqtt_t *mqtt);
/* 调用业务的 connect。 */
void base_runtime_connect(void);
/* 任务是否记在业务名下（经运行时创建的任务和业务连接的任务）。 */
bool base_runtime_owns_task(TaskHandle_t task);
/* 业务在 Profile 里的部分。 */
void base_runtime_snapshot(base_business_profile_t *out);
