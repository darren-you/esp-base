// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_ota_transfer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ebase_mqtt_ota_stream_t s_stream;

/* Capture time after entering the short state critical section, so a caller
 * preempted before lock acquisition cannot manufacture backwards time. No
 * network, authentication, delay or Flash I/O occurs while holding it. */
static uint64_t now_ms(void)
{
    const int64_t now = esp_timer_get_time();
    return now < 0 ? UINT64_MAX : (uint64_t)now / 1000u;
}

bool esp_base_mqtt_ota_transfer_begin(uint32_t size, uint32_t total, uint32_t idle)
{
    portENTER_CRITICAL(&s_lock);
    const bool started = ebase_mqtt_ota_stream_begin(&s_stream, size, now_ms(), total, idle);
    portEXIT_CRITICAL(&s_lock);
    return started;
}

bool esp_base_mqtt_ota_transfer_request(ebase_mqtt_ota_frame_t *out)
{
    portENTER_CRITICAL(&s_lock);
    const bool ready = ebase_mqtt_ota_stream_request(&s_stream, now_ms(), out);
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

ebase_mqtt_ota_admission_t esp_base_mqtt_ota_transfer_offer(const ebase_mqtt_ota_frame_t *verified)
{
    portENTER_CRITICAL(&s_lock);
    const ebase_mqtt_ota_admission_t admitted = ebase_mqtt_ota_stream_offer(&s_stream, now_ms(), verified);
    portEXIT_CRITICAL(&s_lock);
    return admitted;
}

int esp_base_mqtt_ota_transfer_read(void *context, uint8_t *out, size_t capacity,
    uint32_t timeout_ms)
{
    (void)context;
    if (!out || !capacity || !timeout_ms) return -1;
    const int64_t start = esp_timer_get_time();
    if (start < 0 || start > INT64_MAX - (int64_t)timeout_ms * 1000) return -1;
    const int64_t deadline = start + (int64_t)timeout_ms * 1000;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now < start) return -1;
        if (now >= deadline) return -2;
        portENTER_CRITICAL(&s_lock);
        const int result = ebase_mqtt_ota_stream_read(&s_stream, now_ms(), out, capacity);
        portEXIT_CRITICAL(&s_lock);
        if (result != -2) return result;
        /* Yield to the control/MQTT owner outside both the mailbox and Flash
         * gates. Return a retry before scheduling a tick beyond this budget. */
        const int64_t before_delay = esp_timer_get_time();
        if (before_delay < now) return -1;
        if (deadline - before_delay <= (int64_t)portTICK_PERIOD_MS * 1000) return -2;
        vTaskDelay(1);
    }
}

bool esp_base_mqtt_ota_transfer_written(void *context, uint32_t written, uint32_t total)
{
    (void)context;
    portENTER_CRITICAL(&s_lock);
    const bool acknowledged = total == s_stream.size &&
        ebase_mqtt_ota_stream_written(&s_stream, now_ms(), written);
    portEXIT_CRITICAL(&s_lock);
    return acknowledged;
}

bool esp_base_mqtt_ota_transfer_select(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool selected = ebase_mqtt_ota_stream_select(&s_stream, now_ms());
    portEXIT_CRITICAL(&s_lock);
    return selected;
}

void esp_base_mqtt_ota_transfer_close(void)
{
    portENTER_CRITICAL(&s_lock);
    ebase_mqtt_ota_stream_close(&s_stream);
    portEXIT_CRITICAL(&s_lock);
}
