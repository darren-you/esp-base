// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_mqtt_contract.h"
#include "esp_err.h"

typedef struct ebase_mqtt_runtime ebase_mqtt_runtime_t;
typedef enum { EBASE_MQTT_STOPPED, EBASE_MQTT_CONNECTING, EBASE_MQTT_SUBSCRIBING,
               EBASE_MQTT_READY, EBASE_MQTT_DISCONNECTED, EBASE_MQTT_FAILED } ebase_mqtt_state_t;
typedef enum { EBASE_MQTT_ERROR_NONE, EBASE_MQTT_ERROR_TRANSPORT, EBASE_MQTT_ERROR_TLS,
               EBASE_MQTT_ERROR_AUTH, EBASE_MQTT_ERROR_BROKER, EBASE_MQTT_ERROR_SUBSCRIPTION,
               EBASE_MQTT_ERROR_FRAGMENT, EBASE_MQTT_ERROR_QUEUE, EBASE_MQTT_ERROR_EXPIRED,
               EBASE_MQTT_ERROR_SDK } ebase_mqtt_error_t;
typedef enum { EBASE_MQTT_EVENT_CONNECTED, EBASE_MQTT_EVENT_READY, EBASE_MQTT_EVENT_DISCONNECTED,
               EBASE_MQTT_EVENT_MESSAGE, EBASE_MQTT_EVENT_PUBACK, EBASE_MQTT_EVENT_DELETED,
               EBASE_MQTT_EVENT_UNSUBSCRIBED, EBASE_MQTT_EVENT_ERROR } ebase_mqtt_event_kind_t;
typedef struct {
    ebase_mqtt_event_kind_t kind;
    ebase_mqtt_error_t error;
    int message_id;
    int broker_code;
    int tls_flags;
    ebase_mqtt_message_t message;
} ebase_mqtt_event_t;

#define ESP_ERR_EBASE_MQTT_OUTBOX_FULL 0x7501
#define ESP_ERR_EBASE_MQTT_TIME_REQUIRED 0x7502

/* 仅一个实例，由创建它的控制任务调用所有 API。MQTT 回调仅复制有界事件。
 * start/subscribe/stop 可能等待官方 API 锁或网络期限，不得从 SDK 回调调用。
 * poll 的输出由调用者持有，建议静态存储，避免在小任务栈分配 4 KiB。 */
esp_err_t esp_base_mqtt_create(const ebase_mqtt_config_t *config, ebase_mqtt_runtime_t **out);
esp_err_t esp_base_mqtt_start(ebase_mqtt_runtime_t *runtime, bool network_ready, bool trusted_time_ready);
esp_err_t esp_base_mqtt_stop(ebase_mqtt_runtime_t *runtime);
esp_err_t esp_base_mqtt_destroy(ebase_mqtt_runtime_t *runtime);
bool esp_base_mqtt_poll(ebase_mqtt_runtime_t *runtime, ebase_mqtt_event_t *out);
ebase_mqtt_state_t esp_base_mqtt_state(const ebase_mqtt_runtime_t *runtime);
esp_err_t esp_base_mqtt_enqueue(ebase_mqtt_runtime_t *runtime, const char *topic,
                                const void *payload, size_t length, uint8_t qos, bool retain, int *message_id);
esp_err_t esp_base_mqtt_subscribe(ebase_mqtt_runtime_t *runtime, const char *filter, uint8_t qos);
esp_err_t esp_base_mqtt_unsubscribe(ebase_mqtt_runtime_t *runtime, const char *filter);
/* 官方 outbox 的协议字节计数，不是完整 heap 占用或远端处理证明。 */
int esp_base_mqtt_outbox_size(const ebase_mqtt_runtime_t *runtime);
