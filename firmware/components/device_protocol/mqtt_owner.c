// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_owner.h"
#include "esp_base_mqtt_command.h"
#include "emqtt.h"
#include <stdio.h>
#include <string.h>

static emqtt_runtime_t *s_runtime;
static emqtt_event_t s_event;
static emqtt_config_t s_emqtt_config;
static char s_topics[4][EBASE_MQTT_TOPIC_BYTES];
static char s_device_id[37];
static char s_online[192];
static uint8_t s_management_key[EBASE_MQTT_KEY_BYTES];
static bool s_configured, s_started, s_ready, s_failed, s_network_ready;
static uint64_t s_retry_after_ms;

static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
}

esp_err_t esp_base_mqtt_owner_configure(const ebase_mqtt_config_t *config,
                                       const char *device_id, const char *boot_id)
{
    if (!config || !device_id || !boot_id) return ESP_ERR_INVALID_ARG;
    s_ready = false;
    s_network_ready = false;
    if (s_runtime) {
        const esp_err_t stopped = emqtt_destroy(s_runtime);
        if (stopped != ESP_OK) {
            s_failed = true;
            s_configured = false;
            wipe(s_management_key, sizeof s_management_key);
            return stopped;
        }
        s_runtime = NULL;
    }
    s_started = s_failed = s_configured = false;
    s_retry_after_ms = 0;
    wipe(&s_emqtt_config, sizeof s_emqtt_config);
    wipe(s_management_key, sizeof s_management_key);
    memset(s_device_id, 0, sizeof s_device_id);
    if (!config->configured) return ESP_OK;
    for (unsigned channel = 0; channel < 4; ++channel) {
        if (!ebase_mqtt_topic(s_topics[channel], device_id, (ebase_mqtt_channel_t)channel)) {
            s_failed = true; return ESP_ERR_INVALID_ARG;
        }
    }
    const int offline_size = snprintf((char *)s_emqtt_config.will_payload,
        sizeof s_emqtt_config.will_payload,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"state\":\"offline\"}",
        device_id, boot_id);
    const int online_size = snprintf(s_online, sizeof s_online,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"state\":\"online\"}",
        device_id, boot_id);
    if (offline_size <= 0 || (size_t)offline_size >= sizeof s_emqtt_config.will_payload ||
        online_size <= 0 || (size_t)online_size >= sizeof s_online) {
        s_failed = true; return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_emqtt_config.hostname, config->hostname, sizeof config->hostname);
    s_emqtt_config.port = config->port;
    s_emqtt_config.tls = true;
    memcpy(s_emqtt_config.client_id, device_id, strlen(device_id) + 1);
    memcpy(s_emqtt_config.username, config->username, sizeof config->username);
    memcpy(s_emqtt_config.password, config->password, sizeof config->password);
    memcpy(s_emqtt_config.ca_pem, config->ca_pem, sizeof config->ca_pem);
    memcpy(s_emqtt_config.will_topic, s_topics[EBASE_MQTT_STATUS],
           strlen(s_topics[EBASE_MQTT_STATUS]) + 1);
    s_emqtt_config.will_length = (size_t)offline_size;
    s_emqtt_config.will_qos = 1;
    s_emqtt_config.will_retain = true;
    s_emqtt_config.subscription_count = 1;
    memcpy(s_emqtt_config.subscriptions[0].topic, s_topics[EBASE_MQTT_COMMAND],
           strlen(s_topics[EBASE_MQTT_COMMAND]) + 1);
    s_emqtt_config.subscriptions[0].qos = 1;
    memcpy(s_management_key, config->management_key, sizeof s_management_key);
    memcpy(s_device_id, device_id, strlen(device_id) + 1);
    const esp_err_t result = emqtt_create(&s_emqtt_config, &s_runtime);
    wipe(&s_emqtt_config, sizeof s_emqtt_config);
    if (result != ESP_OK) {
        wipe(s_management_key, sizeof s_management_key);
        s_failed = true;
        return result;
    }
    s_configured = true;
    return ESP_OK;
}

void esp_base_mqtt_owner_poll(uint64_t now_ms, bool network_ready, bool trusted_time_ready,
                              ebase_mqtt_command_handler_t handler, void *context)
{
    if (!s_configured || !s_runtime || s_failed) return;
    s_network_ready = network_ready && trusted_time_ready;
    if (!s_network_ready) {
        s_ready = false;
        if (s_started) {
            if (emqtt_stop(s_runtime) != ESP_OK) { s_failed = true; return; }
            s_started = false;
        }
        return;
    }
    if (!s_started) {
        if (now_ms < s_retry_after_ms) return;
        if (emqtt_start(s_runtime, true, true) != ESP_OK) {
            s_retry_after_ms = now_ms + 5000;
            return;
        }
        s_started = true;
    }
    for (unsigned i = 0; i < 8 && emqtt_poll(s_runtime, &s_event); ++i) {
        switch (s_event.kind) {
        case EMQTT_EVENT_READY: {
            int message_id = -1;
            const esp_err_t sent = emqtt_enqueue(s_runtime, s_topics[EBASE_MQTT_STATUS],
                                                  s_online, strlen(s_online), 1, true, &message_id);
            s_ready = sent == ESP_OK;
            if (sent != ESP_OK) {
                if (emqtt_stop(s_runtime) != ESP_OK) s_failed = true;
                else { s_started = false; s_retry_after_ms = now_ms + 5000; }
            }
            break;
        }
        case EMQTT_EVENT_DISCONNECTED:
            s_ready = false;
            break;
        case EMQTT_EVENT_ERROR:
            if (emqtt_state(s_runtime) != EMQTT_READY) s_ready = false;
            if (emqtt_state(s_runtime) == EMQTT_FAILED) {
                if (emqtt_stop(s_runtime) != ESP_OK) s_failed = true;
                else { s_started = false; s_retry_after_ms = now_ms + 5000; }
            }
            break;
        case EMQTT_EVENT_MESSAGE:
            if (s_ready && handler) {
                ebase_mqtt_request_view_t verified;
                if (ebase_mqtt_verified_request(s_management_key, s_device_id,
                        s_event.message.topic, s_event.message.qos, s_event.message.retain,
                        s_event.message.payload, s_event.message.length, &verified))
                    handler(verified.request, verified.request_length, context);
            }
            break;
        default:
            break;
        }
    }
}

const char *esp_base_mqtt_owner_state(void)
{
    if (s_failed) return "failed";
    if (!s_configured) return "unconfigured";
    if (s_ready && s_network_ready) return "ready";
    if (s_started && emqtt_state(s_runtime) == EMQTT_DISCONNECTED) return "disconnected";
    return "connecting";
}

bool esp_base_mqtt_owner_ready(void)
{
    return s_configured && s_runtime && s_started && s_ready && s_network_ready && !s_failed &&
           emqtt_state(s_runtime) == EMQTT_READY;
}

static bool publish(ebase_mqtt_channel_t channel, const char *json, size_t length)
{
    if (!esp_base_mqtt_owner_ready() || !json || !length || length > EMQTT_PAYLOAD_MAX) return false;
    int message_id = -1;
    return emqtt_enqueue(s_runtime, s_topics[channel], json, length, 1, false, &message_id) == ESP_OK;
}

bool esp_base_mqtt_owner_result(const char *json, size_t length)
{
    return publish(EBASE_MQTT_RESULT, json, length);
}

bool esp_base_mqtt_owner_reported(const char *json, size_t length)
{
    return publish(EBASE_MQTT_REPORTED, json, length);
}
