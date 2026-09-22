// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ebase_mqtt_config_t config(void)
{
    ebase_mqtt_config_t c = {.hostname = "broker.example.invalid", .port = 8883, .tls = true,
        .client_id = "11111111-1111-4111-8111-111111111111",
        .ca_pem = "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
        .will_topic = "unit/status", .will_qos = 1,
        .subscriptions = {{.topic = "unit/+/command", .qos = 1}}, .subscription_count = 1};
    return c;
}

int main(void)
{
    ebase_mqtt_config_t c = config();
    assert(ebase_mqtt_config_valid(&c, false));
    c.tls = false; c.ca_pem[0] = 0;
    assert(!ebase_mqtt_config_valid(&c, false));
    assert(ebase_mqtt_config_valid(&c, true));
    const char *bad_hosts[] = {"", "mqtt://broker", "user@broker", "bad host", ".invalid", "a..b", "-a", "a-", "a."};
    for (size_t i = 0; i < sizeof(bad_hosts) / sizeof(bad_hosts[0]); ++i) {
        c = config(); strcpy(c.hostname, bad_hosts[i]); assert(!ebase_mqtt_config_valid(&c, false));
    }
    c = config(); c.client_id[19] = '7'; assert(!ebase_mqtt_config_valid(&c, false));
    c = config(); c.ca_pem[0] = 0; assert(!ebase_mqtt_config_valid(&c, false));
    c = config(); c.port = 0; assert(!ebase_mqtt_config_valid(&c, false));
    c = config(); c.subscription_count = 9; assert(!ebase_mqtt_config_valid(&c, false));
    c = config(); c.subscription_count = 2; c.subscriptions[1] = c.subscriptions[0];
    assert(!ebase_mqtt_config_valid(&c, false));
    const char *bad_filters[] = {"a#", "a/#/b", "a/+b", "a/b+", "a\x01", "a\xc0\x80", "a\xed\xa0\x80"};
    for (size_t i = 0; i < sizeof(bad_filters) / sizeof(bad_filters[0]); ++i)
        assert(!ebase_mqtt_topic_valid(bad_filters[i], strlen(bad_filters[i]), true));
    assert(ebase_mqtt_topic_valid("a/+/设备", strlen("a/+/设备"), true));
    assert(ebase_mqtt_topic_valid("#", 1, true));
    assert(!ebase_mqtt_topic_valid("#", 1, false));
    const uint8_t accepted[] = {1}, rejected[] = {0x80}, high_qos[] = {2}, extra[] = {1, 1};
    c = config();
    assert(ebase_mqtt_suback_valid(accepted, 1, c.subscriptions, 1));
    assert(!ebase_mqtt_suback_valid(rejected, 1, c.subscriptions, 1));
    assert(!ebase_mqtt_suback_valid(high_qos, 1, c.subscriptions, 1));
    assert(!ebase_mqtt_suback_valid(extra, 2, c.subscriptions, 1));
    c.subscriptions[0].qos = 0;
    assert(!ebase_mqtt_suback_valid(accepted, 1, c.subscriptions, 1));

    static ebase_mqtt_receiver_t receiver;
    uint8_t data[EBASE_MQTT_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    ebase_mqtt_fragment_t f = {.topic = "unit/in", .topic_length = 7, .data = data,
        .data_length = 0, .total_length = sizeof(data), .message_id = 45, .qos = 1, .retain = true, .duplicate = true};
    assert(ebase_mqtt_receive(&receiver, &f) == EBASE_MQTT_RX_MORE);
    f.topic = NULL; f.topic_length = 0;
    for (size_t i = 0; i < sizeof(data); ++i) {
        f.offset = (int)i; f.data = data + i; f.data_length = 1;
        assert(ebase_mqtt_receive(&receiver, &f) == (i + 1 == sizeof(data) ? EBASE_MQTT_RX_COMPLETE : EBASE_MQTT_RX_MORE));
    }
    assert(receiver.message.length == sizeof(data) && receiver.message.retain && receiver.message.duplicate);
    assert(!strcmp(receiver.message.topic, "unit/in") && !memcmp(receiver.message.payload, data, sizeof(data)));
    f = (ebase_mqtt_fragment_t){.topic = "unit/in", .topic_length = 7};
    assert(ebase_mqtt_receive(&receiver, &f) == EBASE_MQTT_RX_COMPLETE);
    assert(receiver.message.length == 0);
    f.data = data; f.data_length = 3; f.total_length = 6;
    assert(ebase_mqtt_receive(&receiver, &f) == EBASE_MQTT_RX_MORE);
    f.topic = NULL; f.topic_length = 0; f.offset = 4;
    assert(ebase_mqtt_receive(&receiver, &f) == EBASE_MQTT_RX_REJECTED && !receiver.active);
    f = (ebase_mqtt_fragment_t){.topic = "unit/in", .topic_length = 7, .data = data, .data_length = 1, .total_length = 4097};
    assert(ebase_mqtt_receive(&receiver, &f) == EBASE_MQTT_RX_REJECTED);
    /* 确定性字段畸变只传有效内存；验证负数/边界不会越界拷贝。 */
    uint32_t random = 1;
    for (int i = 0; i < 10000; ++i) {
        random = random * 1664525u + 1013904223u;
        f.offset = (int)(random % 6000u) - 1000;
        f.total_length = (int)((random >> 8) % 6000u) - 1000;
        f.data_length = (int)((random >> 16) % 4000u) - 1000;
        const ebase_mqtt_rx_result_t result = ebase_mqtt_receive(&receiver, &f);
        if (result == EBASE_MQTT_RX_COMPLETE) assert(receiver.message.length <= EBASE_MQTT_PAYLOAD_MAX);
    }
    puts("  mqtt_contract  passed");
    return 0;
}
