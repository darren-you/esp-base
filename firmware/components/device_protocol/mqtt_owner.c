// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_owner.h"
#include "esp_attr.h"
#include "esp_base_mqtt_command.h"
#include "emqtt.h"
#include <stdio.h>
#include <string.h>

static emqtt_runtime_t *s_runtime;
/* Incoming MESSAGE remains live while its handler parses a command. */
static emqtt_event_t s_event IRAM_BSS_ATTR;
static char s_topics[5][EBASE_MQTT_TOPIC_BYTES];
static char s_device_id[37];
static char s_boot_id[37];
static char s_online[192];
static uint8_t s_management_key[EBASE_MQTT_KEY_BYTES];
static bool s_configured, s_started, s_ready, s_failed, s_network_ready;
static uint64_t s_retry_after_ms;
static uint64_t s_event_sequence;
static int s_restart_result_id;
static bool s_restart_result_acknowledged;

static void clear_restart_result(void)
{
    s_restart_result_id = 0;
    s_restart_result_acknowledged = false;
}

static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
}

esp_err_t esp_base_mqtt_owner_revoke(void)
{
    clear_restart_result();
    s_ready = false;
    s_network_ready = false;
    if (s_runtime) {
        const esp_err_t stopped = emqtt_destroy(s_runtime);
        if (stopped != ESP_OK) {
            s_failed = true;
            s_configured = false;
            wipe(s_management_key, sizeof s_management_key);
            memset(s_device_id, 0, sizeof s_device_id);
            return stopped;
        }
        s_runtime = NULL;
    }
    s_started = s_failed = s_configured = false;
    s_retry_after_ms = 0;
    wipe(s_management_key, sizeof s_management_key);
    memset(s_device_id, 0, sizeof s_device_id);
    return ESP_OK;
}

esp_err_t esp_base_mqtt_owner_configure(const ebase_mqtt_config_t *config,
                                       const char *device_id, const char *boot_id,
                                       emqtt_config_t *scratch)
{
    if (!config || !device_id || !boot_id || !scratch) return ESP_ERR_INVALID_ARG;
    wipe(scratch, sizeof *scratch);
    const esp_err_t revoked = esp_base_mqtt_owner_revoke();
    if (revoked != ESP_OK) return revoked;
    if (!config->configured) return ESP_OK;
    for (unsigned channel = 0; channel < 5; ++channel) {
        if (!ebase_mqtt_topic(s_topics[channel], device_id, (ebase_mqtt_channel_t)channel)) {
            s_failed = true; return ESP_ERR_INVALID_ARG;
        }
    }
    const int offline_size = snprintf((char *)scratch->will_payload,
        sizeof scratch->will_payload,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"state\":\"offline\"}",
        device_id, boot_id);
    const int online_size = snprintf(s_online, sizeof s_online,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"state\":\"online\"}",
        device_id, boot_id);
    if (offline_size <= 0 || (size_t)offline_size >= sizeof scratch->will_payload ||
        online_size <= 0 || (size_t)online_size >= sizeof s_online) {
        s_failed = true;
        wipe(scratch, sizeof *scratch);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(scratch->hostname, config->hostname, sizeof config->hostname);
    scratch->port = config->port;
    scratch->tls = true;
    memcpy(scratch->client_id, device_id, strlen(device_id) + 1);
    memcpy(scratch->username, config->username, sizeof config->username);
    memcpy(scratch->password, config->password, sizeof config->password);
    memcpy(scratch->ca_pem, config->ca_pem, sizeof config->ca_pem);
    memcpy(scratch->will_topic, s_topics[EBASE_MQTT_STATUS],
           strlen(s_topics[EBASE_MQTT_STATUS]) + 1);
    scratch->will_length = (size_t)offline_size;
    scratch->will_qos = 1;
    scratch->will_retain = true;
    scratch->subscription_count = 2;
    memcpy(scratch->subscriptions[0].topic, s_topics[EBASE_MQTT_COMMAND],
           strlen(s_topics[EBASE_MQTT_COMMAND]) + 1);
    scratch->subscriptions[0].qos = 1;
    memcpy(scratch->subscriptions[1].topic, s_topics[EBASE_MQTT_EVENT],
           strlen(s_topics[EBASE_MQTT_EVENT]) + 1);
    scratch->subscriptions[1].qos = 1;
    memcpy(s_management_key, config->management_key, sizeof s_management_key);
    memcpy(s_device_id, device_id, strlen(device_id) + 1);
    if (strcmp(s_boot_id, boot_id) != 0) s_event_sequence = 0;
    memcpy(s_boot_id, boot_id, strlen(boot_id) + 1);
    const esp_err_t result = emqtt_create(scratch, &s_runtime);
    wipe(scratch, sizeof *scratch);
    if (result != ESP_OK) {
        wipe(s_management_key, sizeof s_management_key);
        s_failed = true;
        return result;
    }
    s_configured = true;
    return ESP_OK;
}

void esp_base_mqtt_owner_poll(uint64_t now_ms, bool network_ready, bool trusted_time_ready,
                              ebase_mqtt_command_handler_t command_handler,
                              ebase_mqtt_event_handler_t event_handler, void *context)
{
    if (!s_configured || !s_runtime || s_failed) return;
    s_network_ready = network_ready && trusted_time_ready;
    if (!s_network_ready) {
        clear_restart_result();
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
            clear_restart_result();
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
            clear_restart_result();
            s_ready = false;
            break;
        case EMQTT_EVENT_PUBACK:
            if (s_restart_result_id > 0 && s_event.message_id == s_restart_result_id)
                s_restart_result_acknowledged = true;
            break;
        case EMQTT_EVENT_DELETED:
            clear_restart_result();
            /* A QoS 1 outbox entry expired before broker acknowledgement.
             * That publication cannot be reported as delivered. Close this session;
             * the caller may resend the same request_id after a new SUBACK. */
            s_ready = false;
            if (emqtt_stop(s_runtime) != ESP_OK) s_failed = true;
            else { s_started = false; s_retry_after_ms = now_ms + 5000; }
            return;
        case EMQTT_EVENT_ERROR:
            if (emqtt_state(s_runtime) != EMQTT_READY) {
                clear_restart_result();
                s_ready = false;
            }
            if (emqtt_state(s_runtime) == EMQTT_FAILED) {
                if (emqtt_stop(s_runtime) != ESP_OK) s_failed = true;
                else { s_started = false; s_retry_after_ms = now_ms + 5000; }
            }
            break;
        case EMQTT_EVENT_MESSAGE:
            if (s_ready && command_handler) {
                ebase_mqtt_request_view_t verified;
                if (ebase_mqtt_verified_request(s_management_key, s_device_id,
                        s_event.message.topic, s_event.message.qos, s_event.message.retain,
                        s_event.message.payload, s_event.message.length, &verified))
                    command_handler(verified.request, verified.request_length, context);
            }
            if (s_ready && event_handler && s_event_sequence != UINT64_MAX) {
                ebase_mqtt_event_view_t verified;
                if (ebase_mqtt_verified_event(s_management_key, s_device_id,
                        s_boot_id, s_event.message.topic, s_event.message.qos,
                        s_event.message.retain, s_event.message.payload,
                        s_event.message.length, &verified) &&
                    verified.event_sequence == s_event_sequence + 1U &&
                    event_handler(&verified, context)) {
                    s_event_sequence = verified.event_sequence;
                }
            }
            break;
        default:
            break;
        }
    }
}

uint64_t esp_base_mqtt_owner_event_sequence(void)
{
    return s_event_sequence;
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
    if (!esp_base_mqtt_owner_ready() || !json || !length || length > EMQTT_PUBLISH_PAYLOAD_MAX_BYTES) return false;
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

bool esp_base_mqtt_owner_restart_result(const char *json, size_t length)
{
    if (!esp_base_mqtt_owner_ready() || s_restart_result_id != 0 ||
        !json || !length || length > EMQTT_PUBLISH_PAYLOAD_MAX_BYTES) return false;
    int message_id = 0;
    if (emqtt_enqueue(s_runtime, s_topics[EBASE_MQTT_RESULT], json, length,
                      1, false, &message_id) != ESP_OK || message_id <= 0) return false;
    s_restart_result_id = message_id;
    s_restart_result_acknowledged = false;
    return true;
}

bool esp_base_mqtt_owner_restart_result_acknowledged(void)
{
    return s_restart_result_id > 0 && s_restart_result_acknowledged;
}
