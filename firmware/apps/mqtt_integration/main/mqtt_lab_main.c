// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "esp_base_identity.h"
#include "esp_base_remote_config.h"
#include "esp_base_wifi.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mqtt_lab_inputs.h"
#include "mqtt_lab_resources.h"

static emqtt_runtime_t *mqtt;
static emqtt_event_t event;
static esp_base_remote_config_t committed;
static char output_topic[EMQTT_TOPIC_MAX + 1];
static char input_topic[EMQTT_TOPIC_MAX + 1];
static char extra_topic[EMQTT_TOPIC_MAX + 1];
static bool clock_ready;
static unsigned cycles;
static int64_t wifi_restore_at;

static void report(void)
{
    printf("EBASE_MQTT_LAB state=%d cycle=%u heap=%" PRIu32 " min_heap=%" PRIu32
           " largest=%u tasks=%u owner_stack=%u outbox=%d wifi=%s\n",
           mqtt ? (int)emqtt_state(mqtt) : -1, cycles, esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)uxTaskGetNumberOfTasks(),
           (unsigned)uxTaskGetStackHighWaterMark(NULL), emqtt_outbox_size(mqtt), esp_base_wifi_state());
    fflush(stdout);
}

static bool start_client(void)
{
    esp_err_t error = emqtt_create(&lab_mqtt_config, &mqtt);
    if (error == ESP_OK) error = emqtt_start(mqtt, esp_base_wifi_ready(), clock_ready);
    printf("EBASE_MQTT_LAB start_error=%d\n", error);
    return error == ESP_OK;
}

static bool is_command(const char *value)
{
    return !strcmp(event.message.topic, input_topic) && event.message.length == strlen(value) &&
           !memcmp(event.message.payload, value, event.message.length);
}

static void receive(void)
{
    const bool control = is_command(":cycle") || is_command(":stats") || is_command(":restart") ||
                         is_command(":subscribe") || is_command(":unsubscribe") || is_command(":wifi-cycle");
    if (control && (event.message.retain || event.message.duplicate || event.message.qos != 0)) {
        puts("EBASE_MQTT_LAB command_rejected=requires_nonretained_qos0");
        return;
    }
    if (is_command(":cycle")) {
        /* 实验镜像控制字：等待 SDK 收敛后完整释放、重新建立实例。 */
        esp_err_t error = emqtt_destroy(mqtt);
        if (error == ESP_OK) {
            mqtt = NULL; ++cycles;
            vTaskDelay(pdMS_TO_TICKS(100)); /* 让 Idle 完成已退出任务的释放。 */
            ebase_mqtt_lab_report_resources("destroyed", cycles);
            (void)start_client();
        }
        printf("EBASE_MQTT_LAB cycle_error=%d\n", error);
    } else if (is_command(":restart")) {
        esp_err_t error = emqtt_stop(mqtt);
        if (error == ESP_OK) error = emqtt_start(mqtt, esp_base_wifi_ready(), clock_ready);
        printf("EBASE_MQTT_LAB restart_error=%d\n", error);
    } else if (is_command(":subscribe")) {
        printf("EBASE_MQTT_LAB subscribe_error=%d\n", emqtt_subscribe(mqtt, extra_topic, 1));
    } else if (is_command(":unsubscribe")) {
        printf("EBASE_MQTT_LAB unsubscribe_error=%d\n", emqtt_unsubscribe(mqtt, extra_topic));
    } else if (is_command(":wifi-cycle")) {
        /* 只中断本板 station；不是外部 AP 断电，不写持久配置。 */
        const ebase_wifi_config_t disabled = {0};
        const int64_t now = esp_timer_get_time();
        const esp_err_t error = esp_base_wifi_apply(&disabled, (uint64_t)(now / 1000));
        if (error == ESP_OK) wifi_restore_at = now + 5000000;
        printf("EBASE_MQTT_LAB wifi_pause_error=%d\n", error);
    } else if (is_command(":stats")) {
        report();
        ebase_mqtt_lab_report_resources("requested", cycles);
    } else {
        int id = -1;
        const esp_err_t error = emqtt_enqueue(mqtt, output_topic, event.message.payload,
            event.message.length, event.message.qos, event.message.retain, &id);
        printf("EBASE_MQTT_LAB echo_length=%u qos=%u duplicate=%u retain=%u enqueue_error=%d message_id=%d\n",
               (unsigned)event.message.length, (unsigned)event.message.qos, (unsigned)event.message.duplicate,
               (unsigned)event.message.retain, error, id);
    }
}

void app_main(void)
{
    /* 此标记保留在实际固件中；不能作为正常产品/OTA release。 */
    puts("ESP_BASE_LAB_ONLY MQTT_INTEGRATION official=1.1.0");
    esp_err_t error = nvs_flash_init();
    esp_base_identity_t identity = {0};
    if (error == ESP_OK) error = esp_base_identity_read(&identity);
    if (error == ESP_OK) error = esp_base_remote_config_load(&committed);
    if (error != ESP_OK || !committed.wifi.configured) {
        printf("EBASE_MQTT_LAB storage_error=%d wifi_configured=%u; initialization stopped\n", error, (unsigned)committed.wifi.configured);
        return;
    }
    strcpy(lab_mqtt_config.client_id, identity.device_id);
    snprintf(lab_mqtt_config.will_topic, sizeof(lab_mqtt_config.will_topic), "esp-base-lab/%s/status", identity.device_id);
    memcpy(lab_mqtt_config.will_payload, "offline", 7);
    lab_mqtt_config.will_length = 7; lab_mqtt_config.will_qos = 1; lab_mqtt_config.will_retain = true;
    lab_mqtt_config.subscription_count = 1;
    snprintf(lab_mqtt_config.subscriptions[0].topic, sizeof(lab_mqtt_config.subscriptions[0].topic),
             "esp-base-lab/%s/in", identity.device_id);
    lab_mqtt_config.subscriptions[0].qos = 1;
    snprintf(output_topic, sizeof(output_topic), "esp-base-lab/%s/out", identity.device_id);
    strcpy(input_topic, lab_mqtt_config.subscriptions[0].topic);
    snprintf(extra_topic, sizeof(extra_topic), "esp-base-lab/%s/extra", identity.device_id);
    error = esp_base_wifi_start(&committed.wifi);
    if (error != ESP_OK) { printf("EBASE_MQTT_LAB wifi_start_error=%d\n", error); return; }
    if (lab_mqtt_config.tls) {
        if (!lab_ntp_server[0]) { puts("EBASE_MQTT_LAB time_server_required"); return; }
        esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(lab_ntp_server);
        error = esp_netif_sntp_init(&ntp);
        if (error != ESP_OK) { printf("EBASE_MQTT_LAB time_init_error=%d\n", error); return; }
    }
    int64_t next_report = 0;
    bool start_attempted = false;
    while (true) {
        const int64_t now = esp_timer_get_time();
        esp_base_wifi_poll((uint64_t)(now / 1000));
        if (wifi_restore_at && now >= wifi_restore_at) {
            wifi_restore_at = 0;
            printf("EBASE_MQTT_LAB wifi_restore_error=%d\n",
                   esp_base_wifi_apply(&committed.wifi, (uint64_t)(now / 1000)));
        }
        if (lab_mqtt_config.tls && !clock_ready && esp_netif_sntp_sync_wait(0) == ESP_OK && time(NULL) >= 1704067200) clock_ready = true;
        if (!start_attempted && esp_base_wifi_ready() && (!lab_mqtt_config.tls || clock_ready)) {
            start_attempted = true;
            ebase_mqtt_lab_report_resources("before_create", cycles);
            (void)start_client();
        }
        while (mqtt && emqtt_poll(mqtt, &event)) {
            printf("EBASE_MQTT_LAB event=%d error=%d message_id=%d broker_code=%d tls_flags=%d\n",
                   (int)event.kind, (int)event.error, event.message_id, event.broker_code, event.tls_flags);
            if (event.kind == EMQTT_EVENT_READY || event.kind == EMQTT_EVENT_UNSUBSCRIBED) {
                int id = -1;
                error = emqtt_enqueue(mqtt, lab_mqtt_config.will_topic, "online", 6, 1, true, &id);
                printf("EBASE_MQTT_LAB online_enqueue_error=%d message_id=%d\n", error, id);
            }
            if (event.kind == EMQTT_EVENT_MESSAGE) receive();
        }
        if (now >= next_report) { report(); next_report = now + 5000000; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
