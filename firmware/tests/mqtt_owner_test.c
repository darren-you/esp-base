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
static const char event_hex_1[] = "9fb0774308f1438407d8a298468d0c9d067c76897447a2c1d0618fe5429cab8c";
static const char event_hex_2[] = "1f0b65a5d26e51b20b1e2f0cf3cfed8de744b17586a5d4782d8588dbf07175f7";
static const uint8_t event_tag_1[32] = {
    0x9f, 0xb0, 0x77, 0x43, 0x08, 0xf1, 0x43, 0x84, 0x07, 0xd8, 0xa2, 0x98, 0x46, 0x8d, 0x0c, 0x9d, 0x06, 0x7c, 0x76, 0x89, 0x74, 0x47, 0xa2, 0xc1, 0xd0, 0x61, 0x8f, 0xe5, 0x42, 0x9c, 0xab, 0x8c
};
static const uint8_t event_tag_2[32] = {
    0x1f, 0x0b, 0x65, 0xa5, 0xd2, 0x6e, 0x51, 0xb2, 0x0b, 0x1e, 0x2f, 0x0c, 0xf3, 0xcf, 0xed, 0x8d, 0xe7, 0x44, 0xb1, 0x75, 0x86, 0xa5, 0xd4, 0x78, 0x2d, 0x85, 0x88, 0xdb, 0xf0, 0x71, 0x75, 0xf7
};
static uint8_t signed_event[110];
static struct emqtt_runtime { int marker; } runtime;
static emqtt_config_t captured;
static emqtt_config_t scratch;
static emqtt_event_t events[40];
static unsigned event_head, event_tail, creates, starts, stops, destroys, destroy_attempts, sends, commands, business_events;
static unsigned business_event_attempts, capacity_copies;
static bool accept_event = true;
static emqtt_state_t state = EMQTT_STOPPED;
static bool fail_stop, fail_publish;
static char last_topic[EMQTT_TOPIC_MAX + 1], last_payload[EMQTT_PUBLISH_PAYLOAD_MAX_BYTES + 1];
static uint8_t last_qos;
static bool last_retain;
bool emqtt_get_capacity_snapshot(const emqtt_runtime_t *instance, emqtt_capacity_stats_t *out)
{
    assert(instance == &runtime);
    ++capacity_copies;
    *out = (emqtt_capacity_stats_t){.runtime_instance=1,.counters_valid=true};
    return true;
}

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
        ((length == sizeof request - 1 &&
          !memcmp(tag, expected_tag, sizeof expected_tag) &&
          !memcmp(json, request, length)) ||
         (length == sizeof signed_event &&
          !memcmp(json, signed_event, length) &&
          ((!memcmp(tag, event_tag_1, sizeof event_tag_1) &&
            signed_event[sizeof signed_event - 4U] == 1U) ||
           (!memcmp(tag, event_tag_2, sizeof event_tag_2) &&
            signed_event[sizeof signed_event - 4U] == 2U))));
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
    assert(instance == &runtime && topic && payload && length <= EMQTT_PUBLISH_PAYLOAD_MAX_BYTES && message_id);
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
static void push_business_event(uint8_t sequence, bool retained, bool bad_tag)
{
    assert(sequence == 1U || sequence == 2U);
    assert(event_tail < sizeof events / sizeof events[0]);
    emqtt_event_t *event = &events[event_tail++];
    *event = (emqtt_event_t){.kind = EMQTT_EVENT_MESSAGE};
    strcpy(event->message.topic,
           "esp-base/22222222-2222-4222-8222-222222222222/event");
    const char *hex = sequence == 1U ? event_hex_1 : event_hex_2;
    memcpy(event->message.payload, hex, 64);
    if (bad_tag) event->message.payload[0] ^= 1U;
    event->message.payload[64] = '\n';
    size_t offset = 0;
    static const char domain[] = "esp-base-business-event-v1\n";
    memcpy(signed_event + offset, domain, sizeof domain - 1);
    offset += sizeof domain - 1;
    memcpy(signed_event + offset, device_id, 36);
    offset += 36;
    memcpy(signed_event + offset, boot_id, 36);
    offset += 36;
    memset(signed_event + offset, 0, 8);
    signed_event[offset + 7] = sequence;
    offset += 8;
    memcpy(signed_event + offset, "\x01\x02\x03", 3);
    offset += 3;
    assert(offset == sizeof signed_event);
    memcpy(event->message.payload + 65, signed_event, sizeof signed_event);
    event->message.length = 65 + sizeof signed_event;
    event->message.qos = 1;
    event->message.retain = retained;
}
static void received(const uint8_t *json, size_t length, void *context)
{
    assert(context == &runtime);
    assert(length == sizeof request - 1 && !memcmp(json, request, length));
    ++commands;
}
static bool received_event(const ebase_mqtt_event_view_t *event, void *context)
{
    assert(context == &runtime && event != NULL);
    assert(event->event_sequence == business_events + 1U &&
           event->event_size_bytes == 3U &&
           !memcmp(event->event, "\x01\x02\x03", 3));
    ++business_event_attempts;
    if (accept_event) ++business_events;
    return accept_event;
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

static esp_err_t configure(const ebase_mqtt_config_t *mqtt)
{
    memset(&scratch, 0xa5, sizeof scratch);
    const esp_err_t result = esp_base_mqtt_owner_configure(mqtt, device_id, boot_id, &scratch);
    const uint8_t *bytes = (const uint8_t *)&scratch;
    for (size_t i = 0; i < sizeof scratch; ++i) assert(bytes[i] == 0);
    return result;
}

int main(void)
{
    ebase_mqtt_config_t absent = {0};
    assert(configure(&absent) == ESP_OK);
    esp_base_mqtt_owner_poll(0, true, true, received, received_event, &runtime);
    assert(!creates && !starts && !esp_base_mqtt_owner_ready());
    assert(!strcmp(esp_base_mqtt_owner_state(), "unconfigured"));
    emqtt_capacity_stats_t capacity;
    memset(&capacity, 0xff, sizeof capacity);
    assert(!esp_base_mqtt_owner_capacity(NULL));
    assert(!esp_base_mqtt_owner_capacity(&capacity) && !capacity_copies && !capacity.runtime_instance);

    ebase_mqtt_config_t mqtt = config();
    assert(configure(&mqtt) == ESP_OK);
    assert(creates == 1 && !strcmp(captured.client_id, device_id));
    assert(esp_base_mqtt_owner_capacity(&capacity) && capacity_copies == 1 &&
           capacity.runtime_instance == 1 && capacity.counters_valid && sends == 0 && starts == 0);
    assert(captured.tls && captured.port == 8883 && !strcmp(captured.hostname, mqtt.hostname));
    assert(!strcmp(captured.username, mqtt.username) && !strcmp(captured.password, mqtt.password));
    assert(!strcmp(captured.ca_pem, mqtt.ca_pem));
    assert(captured.subscription_count == 2 && captured.subscriptions[0].qos == 1 &&
           captured.subscriptions[1].qos == 1);
    assert(!strcmp(captured.subscriptions[0].topic, "esp-base/22222222-2222-4222-8222-222222222222/command"));
    assert(!strcmp(captured.subscriptions[1].topic, "esp-base/22222222-2222-4222-8222-222222222222/event"));
    assert(!strcmp(captured.will_topic, "esp-base/22222222-2222-4222-8222-222222222222/status"));
    assert(captured.will_qos == 1 && captured.will_retain);
    assert(!strcmp((const char *)captured.will_payload,
        "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\",\"boot_id\":\"33333333-3333-4333-8333-333333333333\",\"state\":\"offline\"}"));
    assert(captured.will_length == strlen((const char *)captured.will_payload));
    esp_base_mqtt_owner_poll(0, true, false, received, received_event, &runtime);
    esp_base_mqtt_owner_poll(0, false, true, received, received_event, &runtime);
    assert(starts == 0 && !esp_base_mqtt_owner_ready());
    esp_base_mqtt_owner_poll(0, true, true, received, received_event, &runtime);
    assert(starts == 1 && !esp_base_mqtt_owner_ready());
    assert(!esp_base_mqtt_owner_result("{}", 2));
    push_command(captured.subscriptions[0].topic, false, false);
    push(EMQTT_EVENT_PUBACK);
    esp_base_mqtt_owner_poll(0, true, true, received, received_event, &runtime);
    assert(commands == 0 && !esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_ready() && sends == 1 && last_qos == 1 && last_retain);
    assert(!strcmp(last_topic, captured.will_topic));
    assert(!strcmp(last_payload,
        "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\",\"boot_id\":\"33333333-3333-4333-8333-333333333333\",\"state\":\"online\"}"));

    /* A verified frame rejected by the handler consumes no sequence. */
    accept_event = false;
    push_business_event(1U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 0U && business_event_attempts == 1U &&
           esp_base_mqtt_owner_event_sequence() == 0U);
    push_business_event(2U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 0U && business_event_attempts == 1U &&
           esp_base_mqtt_owner_event_sequence() == 0U);
    push_business_event(1U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 0U && business_event_attempts == 2U &&
           esp_base_mqtt_owner_event_sequence() == 0U);
    accept_event = true;
    push_business_event(1U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 1U && business_event_attempts == 3U &&
           esp_base_mqtt_owner_event_sequence() == 1U);
    push_business_event(1U, false, false);
    push_business_event(1U, true, false);
    push_business_event(1U, false, true);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 1U && business_event_attempts == 3U &&
           esp_base_mqtt_owner_event_sequence() == 1U);
    accept_event = false;
    push_business_event(2U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 1U && business_event_attempts == 4U &&
           esp_base_mqtt_owner_event_sequence() == 1U);
    push_business_event(2U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 1U && business_event_attempts == 5U &&
           esp_base_mqtt_owner_event_sequence() == 1U);
    accept_event = true;
    push_business_event(2U, false, false);
    esp_base_mqtt_owner_poll(1, true, true, received, received_event, &runtime);
    assert(business_events == 2U && business_event_attempts == 6U &&
           esp_base_mqtt_owner_event_sequence() == 2U);

    push_command(captured.subscriptions[0].topic, false, false);
    push_command(captured.subscriptions[0].topic, true, false);
    push_command(captured.subscriptions[0].topic, false, true);
    push_command("esp-base/22222222-2222-4222-8222-222222222222/result", false, false);
    esp_base_mqtt_owner_poll(2, true, true, received, received_event, &runtime);
    assert(commands == 1);
    assert(esp_base_mqtt_owner_result("{\"state\":\"succeeded\"}", strlen("{\"state\":\"succeeded\"}")));
    char maximum_result[EMQTT_PUBLISH_PAYLOAD_MAX_BYTES];
    memset(maximum_result, 'v', sizeof maximum_result);
    assert(esp_base_mqtt_owner_result(maximum_result, sizeof maximum_result));
    assert(strlen(last_payload) == sizeof maximum_result &&
           memcmp(last_payload, maximum_result, sizeof maximum_result) == 0);
    assert(!esp_base_mqtt_owner_result(maximum_result, sizeof maximum_result + 1U));
    assert(!strcmp(last_topic, "esp-base/22222222-2222-4222-8222-222222222222/result"));
    assert(last_qos == 1 && !last_retain);
    assert(esp_base_mqtt_owner_reported("{}", 2));
    assert(!strcmp(last_topic, "esp-base/22222222-2222-4222-8222-222222222222/reported"));
    assert(last_qos == 1 && !last_retain);
    assert(!esp_base_mqtt_owner_restart_result(NULL, 2));
    assert(!esp_base_mqtt_owner_restart_result("{}", 0));
    assert(!esp_base_mqtt_owner_restart_result(maximum_result, sizeof maximum_result + 1U));
    fail_publish = true;
    assert(!esp_base_mqtt_owner_result("{}", 2));
    assert(!esp_base_mqtt_owner_restart_result("{}", 2));
    fail_publish = false;
    assert(esp_base_mqtt_owner_restart_result("{}", 2));
    const int restart_result_id = (int)sends;
    assert(!esp_base_mqtt_owner_restart_result_acknowledged());
    assert(!esp_base_mqtt_owner_restart_result("{}", 2));
    assert(esp_base_mqtt_owner_result("{}", 2));
    push(EMQTT_EVENT_PUBACK);
    events[event_tail - 1].message_id = (int)sends;
    esp_base_mqtt_owner_poll(2, true, true, received, received_event, &runtime);
    assert(!esp_base_mqtt_owner_restart_result_acknowledged());
    push(EMQTT_EVENT_PUBACK);
    events[event_tail - 1].message_id = restart_result_id;
    esp_base_mqtt_owner_poll(2, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_restart_result_acknowledged());

    push(EMQTT_EVENT_DISCONNECTED);
    esp_base_mqtt_owner_poll(3, true, true, received, received_event, &runtime);
    assert(!esp_base_mqtt_owner_ready());
    assert(!esp_base_mqtt_owner_restart_result_acknowledged());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(4, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_ready());
    assert(esp_base_mqtt_owner_restart_result("{}", 2));
    esp_base_mqtt_owner_poll(5, false, true, received, received_event, &runtime);
    assert(stops == 1 && !esp_base_mqtt_owner_ready());
    assert(!esp_base_mqtt_owner_restart_result_acknowledged());
    esp_base_mqtt_owner_poll(6, true, true, received, received_event, &runtime);
    assert(starts == 2 && !esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(7, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_ready());

    push(EMQTT_EVENT_ERROR);
    events[event_tail - 1].error = EMQTT_ERROR_SUBSCRIPTION;
    esp_base_mqtt_owner_poll(8, true, true, received, received_event, &runtime);
    assert(stops == 2 && !esp_base_mqtt_owner_ready());
    esp_base_mqtt_owner_poll(5007, true, true, received, received_event, &runtime);
    assert(starts == 2);
    esp_base_mqtt_owner_poll(5008, true, true, received, received_event, &runtime);
    assert(starts == 3);
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(5009, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_ready());

    /* A complete MESSAGE is consumed before the next revision configures
     * its copied client connection. */
    push_command(captured.subscriptions[0].topic, false, false);
    esp_base_mqtt_owner_poll(5009, true, true, received, received_event, &runtime);
    assert(commands == 2);
    assert(configure(&absent) == ESP_OK);
    assert(destroys == 1 && !esp_base_mqtt_owner_ready() && !strcmp(esp_base_mqtt_owner_state(), "unconfigured"));
    assert(configure(&mqtt) == ESP_OK);
    assert(esp_base_mqtt_owner_event_sequence() == 2U);
    assert(!strcmp(captured.ca_pem, mqtt.ca_pem));
    esp_base_mqtt_owner_poll(5010, true, true, received, received_event, &runtime);
    fail_publish = true;
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(5011, true, true, received, received_event, &runtime);
    assert(stops == 4 && !esp_base_mqtt_owner_ready());
    fail_publish = false;
    esp_base_mqtt_owner_poll(10010, true, true, received, received_event, &runtime);
    assert(starts == 4);
    esp_base_mqtt_owner_poll(10011, true, true, received, received_event, &runtime);
    assert(starts == 5);
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(10012, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_ready());
    assert(esp_base_mqtt_owner_result("{\"state\":\"succeeded\"}", strlen("{\"state\":\"succeeded\"}")));
    const int expired_result_id = (int)sends;
    push(EMQTT_EVENT_DELETED);
    events[event_tail - 1].message_id = expired_result_id;
    push_command(captured.subscriptions[0].topic, false, false);
    esp_base_mqtt_owner_poll(10013, true, true, received, received_event, &runtime);
    assert(stops == 5 && commands == 2 && !esp_base_mqtt_owner_ready());
    assert(!esp_base_mqtt_owner_result("{}", 2));
    esp_base_mqtt_owner_poll(15012, true, true, received, received_event, &runtime);
    assert(starts == 5);
    esp_base_mqtt_owner_poll(15013, true, true, received, received_event, &runtime);
    assert(starts == 6 && !esp_base_mqtt_owner_ready());
    push(EMQTT_EVENT_READY);
    esp_base_mqtt_owner_poll(15014, true, true, received, received_event, &runtime);
    assert(esp_base_mqtt_owner_ready());

    fail_stop = true;
    ebase_mqtt_config_t changed = mqtt;
    changed.management_key[0] ^= 1;
    assert(configure(&changed) == ESP_FAIL);
    assert(destroy_attempts == 2 && destroys == 1 && creates == 2);
    push_command(captured.subscriptions[0].topic, false, false);
    esp_base_mqtt_owner_poll(15015, true, true, received, received_event, &runtime);
    assert(commands == 2 && !esp_base_mqtt_owner_result("{}", 2));
    assert(!strcmp(esp_base_mqtt_owner_state(), "failed") && !esp_base_mqtt_owner_ready());
    fail_stop = false;
    assert(esp_base_mqtt_owner_revoke() == ESP_OK);
    assert(!esp_base_mqtt_owner_restart_result_acknowledged());
    assert(destroys == 2 && !strcmp(esp_base_mqtt_owner_state(), "unconfigured") &&
           !esp_base_mqtt_owner_result("{}", 2));
    puts("mqtt_owner passed (TLS/UUID, SUBACK gate, auth, retain, reconnect, outbox expiry, fail closed)");
}
