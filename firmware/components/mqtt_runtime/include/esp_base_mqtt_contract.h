// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBASE_MQTT_PAYLOAD_MAX 4096u
#define EBASE_MQTT_TOPIC_MAX 256u
#define EBASE_MQTT_SUBSCRIPTIONS_MAX 8u
#define EBASE_MQTT_CA_MAX 4096u
#define EBASE_MQTT_OUTBOX_LIMIT 16384u

typedef struct {
    char topic[EBASE_MQTT_TOPIC_MAX + 1];
    uint8_t qos;
} ebase_mqtt_subscription_t;

/* 按值保存输入；证书不借用调用者的临时缓冲区。没有 URI、跳过校验或备用地址。 */
typedef struct {
    char hostname[254];
    uint16_t port;
    bool tls;
    char client_id[37];
    char username[129];
    char password[257];
    char ca_pem[EBASE_MQTT_CA_MAX + 1];
    char will_topic[EBASE_MQTT_TOPIC_MAX + 1];
    uint8_t will_payload[512];
    size_t will_length;
    uint8_t will_qos;
    bool will_retain;
    ebase_mqtt_subscription_t subscriptions[EBASE_MQTT_SUBSCRIPTIONS_MAX];
    size_t subscription_count;
} ebase_mqtt_config_t;

typedef struct {
    char topic[EBASE_MQTT_TOPIC_MAX + 1];
    uint8_t payload[EBASE_MQTT_PAYLOAD_MAX];
    size_t length;
    int message_id;
    uint8_t qos;
    bool retain;
    bool duplicate;
} ebase_mqtt_message_t;

/* 官方 MQTT_EVENT_DATA 的借用字段；此类型不是 MQTT 报文 parser。 */
typedef struct {
    const char *topic;
    int topic_length;
    const void *data;
    int data_length;
    int total_length;
    int offset;
    int message_id;
    int qos;
    bool retain;
    bool duplicate;
} ebase_mqtt_fragment_t;

typedef struct {
    ebase_mqtt_message_t message;
    size_t received;
    bool active;
} ebase_mqtt_receiver_t;

typedef enum {
    EBASE_MQTT_RX_MORE,
    EBASE_MQTT_RX_COMPLETE,
    EBASE_MQTT_RX_REJECTED
} ebase_mqtt_rx_result_t;

bool ebase_mqtt_topic_valid(const char *topic, size_t length, bool filter);
bool ebase_mqtt_config_valid(const ebase_mqtt_config_t *config, bool allow_plaintext_lab);
/* 完整数据归 receiver 所有，只在 COMPLETE 时可消费；下一片会复用缓冲区。 */
ebase_mqtt_rx_result_t ebase_mqtt_receive(ebase_mqtt_receiver_t *receiver, const ebase_mqtt_fragment_t *fragment);
void ebase_mqtt_receive_reset(ebase_mqtt_receiver_t *receiver);
/* 每个返回码必须与本次订阅逐项对应；任何拒绝、截断或额外项均失败。 */
bool ebase_mqtt_suback_valid(const uint8_t *codes, size_t count,
                            const ebase_mqtt_subscription_t *subscriptions, size_t expected_count);
