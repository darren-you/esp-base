// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_owner.h"
#include "esp_base_mqtt_command.h"
#include "emqtt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char device_id[] = "22222222-2222-4222-8222-222222222222";
static const char boot_id[] = "33333333-3333-4333-8333-333333333333";
static const char request[] = "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-111111111111\",\"command\":\"status\"}";
static const char hex_tag[] = "57d8e98b33e69b075cd138712813411c036f615a240e04a54e8c54f2fa3f38ca";
static struct emqtt_runtime { int marker; } runtime;
static emqtt_config_t captured;
static emqtt_event_t events[24];
static unsigned event_head, event_tail, creates, starts, stops, destroys, destroy_attempts, sends, commands;
static emqtt_state_t state = EMQTT_STOPPED;
static bool fail_stop, fail_publish;
static char last_topic[EMQTT_TOPIC_MAX + 1], last_payload[EMQTT_PAYLOAD_MAX + 1];
static uint8_t last_qos;
static bool last_retain;

bool ebase_management_authenticate(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES], const uint8_t *json, size_t length)
{
    static const uint8_t expected_tag[] = {
        0x57, 0xd8, 0xe9, 0x8b, 0x33, 0xe6, 0x9b, 0x07,
        0x5c, 0xd1, 0x38, 0x71, 0x28, 0x13, 0x41, 0x1c,
        0x03, 0x6f, 0x61, 0x5a, 0x24, 0x0e, 0x04, 0xa5,
        0x4e, 0x8c, 0x54, 0xf2, 0xfa, 0x3f, 0x38, 0xca
    };
    uint8_t expected_key[32];
    for (size_t i = 0; i < sizeof expected_key; ++i) expected_key[i] = (uint8_t)i;
    return !memcmp(key, expected_key, sizeof expected_key) &&
        !memcmp(tag, expected_tag, sizeof expected_tag) &&
        length == sizeof request - 1 && !memcmp(json, request, length);
}

esp_err_t emqtt_create(const emqtt_config_t *config, emqtt_runtime_t **out)
{
    assert(emqtt_config_valid(config, false));
    captured = *config;
    ++creates;
    *out = &runtime;
    return ESP_OK;
}
esp_err_t emqtt_start(emqtt_runtime_t *instance, bool network, bool time)
{
    assert(instance == &runtime && network && time);
    ++starts;
    state = EMQTT_CONNECTING;
    return ESP_OK;
}
esp_err_t emqtt_stop(emqtt_runtime_t *instance)
{
    assert(instance == &runtime);
    ++stops;
    if (fail_stop) return ESP_FAIL;
    state = EMQTT_STOPPED;
    event_head = event_tail; /* emqtt_stop drains queued SDK notices. */
    return ESP_OK;
}
esp_err_t emqtt_destroy(emqtt_runtime_t *instance)
{
    assert(instance == &runtime);
    ++destroy_attempts;
    const esp_err_t stopped = emqtt_stop(instance);
    if (stopped != ESP_OK) return stopped;
    ++destroys;
    return ESP_OK;
}
bool emqtt_poll(emqtt_runtime_t *instance, emqtt_event_t *out)
{
    assert(instance == &runtime);
    if (event_head == event_tail) return false;
    *out = events[event_head++];
    if (out->kind == EMQTT_EVENT_READY) state = EMQTT_READY;
    if (out->kind == EMQTT_EVENT_DISCONNECTED) state = EMQTT_DISCONNECTED;
    if (out->kind == EMQTT_EVENT_ERROR && out->error == EMQTT_ERROR_SUBSCRIPTION) state = EMQTT_FAILED;
    return true;
}
emqtt_state_t emqtt_state(const emqtt_runtime_t *instance)
{
    assert(instance == &runtime);
    return state;
}
esp_err_t emqtt_enqueue(emqtt_runtime_t *instance, const char *topic,
    const void *payload, size_t length, uint8_t qos, bool retain, int *message_id)
{
    assert(instance == &runtime && topic && payload && length <= EMQTT_PAYLOAD_MAX && message_id);
    ++sends;
    strcpy(last_topic, topic);
    memcpy(last_payload, payload, length);
    last_payload[length] = '\0';
    last_qos = qos;
    last_retain = retain;
    *message_id = (int)sends;
    return fail_publish ? ESP_FAIL : ESP_OK;
}

static void push(emqtt_event_kind_t kind)
{
    assert(event_tail < sizeof events / sizeof events[0]);
    events[event_tail++] = (emqtt_event_t){.kind = kind};
}
static void push_command(const char *topic, bool retained, bool bad_tag)
{
    assert(event_tail < sizeof events / sizeof events[0]);
    emqtt_event_t *event = &events[event_tail++];
    *event = (emqtt_event_t){.kind = EMQTT_EVENT_MESSAGE};
    strcpy(event->message.topic, topic);
    memcpy(event->message.payload, hex_tag, 64);
    if (bad_tag) event->message.payload[0] = '0';
    event->message.payload[64] = '\n';
    memcpy(event->message.payload + 65, request, sizeof request - 1);
    event->message.length = 65 + sizeof request - 1;
    event->message.qos = 1;
    event->message.retain = retained;
}
static void received(const uint8_t *json, size_t length, void *context)
{
    assert(context == &runtime);
    assert(length == sizeof request - 1 && !memcmp(json, request, length));
    ++commands;
}
static ebase_mqtt_config_t config(void)
{
    ebase_mqtt_config_t result = {.configured = true, .port = 8883};
    strcpy(result.hostname, "broker.example.com");
    strcpy(result.username, "device-principal");
    strcpy(result.password, "test-password");
    strcpy(result.ca_pem, "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n");
    for (size_t i = 0; i < 32; ++i) result.management_key[i] = (uint8_t)i;
    return result;
}

int main(void)
{
    ebase_mqtt_config_t absent = {0};
    assert(esp_base_mqtt_owner_configure(&absent, device_id, boot_id) == ESP_OK);
    esp_base_mqtt_owner_poll(0, true, true, received, &runtime);
    assert(!creates && !starts && !esp_base_mqtt_owner_ready());
    assert(!strcmp(esp_base_mqtt_owner_state(), "unconfigured"));

    ebase_mqtt_config_t mqtt = config();
    assert(esp_base_mqtt_owner_configure(&mqtt, device_id, boot_id) == ESP_OK);
    assert(creates == 1 && !strcmp(captured.client_id, device_id));
    assert(captured.tls && captured.port == 8883 && !strcmp(captured.hostname, mqtt.hostname));
    assert(!strcmp(captured.username, mqtt.username) && !strcmp(captured.password, mqtt.password));
    assert(!strcmp(captured.ca_pem, mqtt.ca_pem));
    assert(captured.subscription_count == 1 && captured.subscriptions[0].qos == 1);
    assert(!strcmp(captured.subscriptions[0].topic, "esp-base/22222222-2222-4222-8222-222222222222/command"));
    assert(!strcmp(captured.will_topic, "esp-base/22222222-2222-4222-8222-222222222222/status"));
    assert(captured.will_qos == 1 && captured.will_retain);
    assert(!strcmp((const char *)captured.will_payload,
        "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\",\"boot_id\":\"33333333-3333-4333-8333-333333333333\",\"state\":\"offline\"}"));
    assert(captured.will_length == strlen((const char *)captured.will_payload));
    esp_base_mqtt_owner_poll(0, true, false, received, &runtime);
    esp_base_mqtt_owner_poll(0, false, true, received, &runtime);
    assert(starts == 0 && !esp_base_mqtt_owner_ready());
    esp_base_mqtt_owner_poll(0, true, true, received, &runtime);
    assert(starts == 1 && !esp_base_mqtt_owner_ready());
    assert(!esp_base_mqtt_owner_result("{}", 2));
    push_command(captured.subscriptions[0].topic, false, false);
    push(EMQTT_EVENT_PUBACK);
    esp_base_mqtt_owner_poll(0, true, true, received, &runtime);
    assert(commands == 0 && !esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(1, true, true, received, &runtime);
    assert(esp_base_mqtt_owner_ready() && sends == 1 && last_qos == 1 && last_retain);
    assert(!strcmp(last_topic, captured.will_topic));
    assert(!strcmp(last_payload,
        "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\",\"boot_id\":\"33333333-3333-4333-8333-333333333333\",\"state\":\"online\"}"));

    push_command(captured.subscriptions[0].topic, false, false);
    push_command(captured.subscriptions[0].topic, true, false);
    push_command(captured.subscriptions[0].topic, false, true);
    push_command("esp-base/22222222-2222-4222-8222-222222222222/result", false, false);
    esp_base_mqtt_owner_poll(2, true, true, received, &runtime);
    assert(commands == 1);
    assert(esp_base_mqtt_owner_result("{\"state\":\"succeeded\"}", strlen("{\"state\":\"succeeded\"}")));
    assert(!strcmp(last_topic, "esp-base/22222222-2222-4222-8222-222222222222/result"));
    assert(last_qos == 1 && !last_retain);
    assert(esp_base_mqtt_owner_reported("{}", 2));
    assert(!strcmp(last_topic, "esp-base/22222222-2222-4222-8222-222222222222/reported"));
    assert(last_qos == 1 && !last_retain);
    fail_publish = true;
    assert(!esp_base_mqtt_owner_result("{}", 2));
    fail_publish = false;

    push(EMQTT_EVENT_DISCONNECTED);
    esp_base_mqtt_owner_poll(3, true, true, received, &runtime);
    assert(!esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(4, true, true, received, &runtime);
    assert(esp_base_mqtt_owner_ready());
    esp_base_mqtt_owner_poll(5, false, true, received, &runtime);
    assert(stops == 1 && !esp_base_mqtt_owner_ready());
    esp_base_mqtt_owner_poll(6, true, true, received, &runtime);
    assert(starts == 2 && !esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(7, true, true, received, &runtime);
    assert(esp_base_mqtt_owner_ready());

    push(EMQTT_EVENT_ERROR);
    events[event_tail - 1].error = EMQTT_ERROR_SUBSCRIPTION;
    esp_base_mqtt_owner_poll(8, true, true, received, &runtime);
    assert(stops == 2 && !esp_base_mqtt_owner_ready());
    esp_base_mqtt_owner_poll(5007, true, true, received, &runtime);
    assert(starts == 2);
    esp_base_mqtt_owner_poll(5008, true, true, received, &runtime);
    assert(starts == 3);
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(5009, true, true, received, &runtime);
    assert(esp_base_mqtt_owner_ready());

    assert(esp_base_mqtt_owner_configure(&absent, device_id, boot_id) == ESP_OK);
    assert(destroys == 1 && !esp_base_mqtt_owner_ready() && !strcmp(esp_base_mqtt_owner_state(), "unconfigured"));
    assert(esp_base_mqtt_owner_configure(&mqtt, device_id, boot_id) == ESP_OK);
    esp_base_mqtt_owner_poll(5010, true, true, received, &runtime);
    fail_publish = true;
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(5011, true, true, received, &runtime);
    assert(stops == 4 && !esp_base_mqtt_owner_ready());
    fail_publish = false;
    esp_base_mqtt_owner_poll(10010, true, true, received, &runtime);
    assert(starts == 4);
    esp_base_mqtt_owner_poll(10011, true, true, received, &runtime);
    assert(starts == 5);
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(10012, true, true, received, &runtime);
    assert(esp_base_mqtt_owner_ready());
    assert(esp_base_mqtt_owner_result("{\"state\":\"succeeded\"}", strlen("{\"state\":\"succeeded\"}")));
    const int expired_result_id = (int)sends;
    push(EMQTT_EVENT_DELETED);
    events[event_tail - 1].message_id = expired_result_id;
    push_command(captured.subscriptions[0].topic, false, false);
    esp_base_mqtt_owner_poll(10013, true, true, received, &runtime);
    assert(stops == 5 && commands == 1 && !esp_base_mqtt_owner_ready());
    assert(!esp_base_mqtt_owner_result("{}", 2));
    esp_base_mqtt_owner_poll(15012, true, true, received, &runtime);
    assert(starts == 5);
    esp_base_mqtt_owner_poll(15013, true, true, received, &runtime);
    assert(starts == 6 && !esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(15014, true, true, received, &runtime);
    assert(esp_base_mqtt_owner_ready());

    fail_stop = true;
    ebase_mqtt_config_t changed = mqtt;
    changed.management_key[0] ^= 1;
    assert(esp_base_mqtt_owner_configure(&changed, device_id, boot_id) == ESP_FAIL);
    assert(destroy_attempts == 2 && destroys == 1 && creates == 2);
    push_command(captured.subscriptions[0].topic, false, false);
    esp_base_mqtt_owner_poll(15015, true, true, received, &runtime);
    assert(commands == 1 && !esp_base_mqtt_owner_result("{}", 2));
    assert(!strcmp(esp_base_mqtt_owner_state(), "failed") && !esp_base_mqtt_owner_ready());
    puts("mqtt_owner passed (TLS/UUID, SUBACK gate, auth, retain, reconnect, outbox expiry, fail closed)");
}
