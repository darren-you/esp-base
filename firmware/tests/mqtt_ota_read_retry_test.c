// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_ota_transfer.h"
#include "freertos/task.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int64_t now_us, wakeup_delay_us, clock_values[4];
static size_t clock_count, clock_index;
static unsigned critical_depth, delays;
static bool offer_during_delay;
static uint8_t block[2048];

int64_t esp_timer_get_time(void)
{
    if (clock_index < clock_count) return clock_values[clock_index++];
    return now_us;
}
void test_mqtt_critical_enter(portMUX_TYPE *lock)
{
    assert(lock && critical_depth == 0);
    ++critical_depth;
}
void test_mqtt_critical_exit(portMUX_TYPE *lock)
{
    assert(lock && critical_depth == 1);
    --critical_depth;
}
static ebase_mqtt_ota_frame_t request(void)
{
    ebase_mqtt_ota_frame_t frame;
    assert(esp_base_mqtt_ota_transfer_request(&frame));
    return frame;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1 && critical_depth == 0 && ++delays == 1);
    now_us += (int64_t)ticks * portTICK_PERIOD_MS * 1000 + wakeup_delay_us;
    if (offer_during_delay) {
        ebase_mqtt_ota_frame_t frame = request();
        frame.kind = EBASE_MQTT_OTA_DATA;
        frame.data = block;
        frame.length = sizeof block;
        assert(esp_base_mqtt_ota_transfer_offer(&frame) == EBASE_MQTT_OTA_ACCEPTED);
        offer_during_delay = false;
    }
}
static void begin(uint32_t size, uint32_t idle)
{
    assert(critical_depth == 0);
    esp_base_mqtt_ota_transfer_close();
    now_us = wakeup_delay_us = 0;
    clock_count = clock_index = 0;
    delays = 0;
    offer_during_delay = false;
    assert(esp_base_mqtt_ota_transfer_begin(size, idle));
}
static int read_once(uint8_t *out, size_t capacity, uint32_t timeout)
{
    delays = 0;
    const int count = esp_base_mqtt_ota_transfer_read(NULL, out, capacity, timeout);
    assert(critical_depth == 0 && delays <= 1);
    return count;
}
static void test_cross_call_wait(void)
{
    uint8_t bytes[64];
    begin(4096, 30000);
    for (unsigned i = 0; i < 120; ++i) {
        assert(read_once(bytes, sizeof bytes, 1000) == -2 && delays == 1);
        assert(now_us == (int64_t)(i + 1) * 10000);
    }
    ebase_mqtt_ota_frame_t frame = request();
    assert(frame.offset == 0 && frame.written_offset == 0 && frame.expires_at_uptime_ms == 30000);
    /* Data arriving during the yielded tick is consumed only on later calls. */
    memset(bytes, 0xa5, sizeof bytes);
    offer_during_delay = true;
    wakeup_delay_us = 10001;
    const int64_t started = now_us;
    assert(read_once(bytes, sizeof bytes, 1000) == -2 && delays == 1);
    assert(now_us - started == 20001);
    for (size_t i = 0; i < sizeof bytes; ++i) assert(bytes[i] == 0xa5);
    for (unsigned offset = 0; offset < sizeof block; offset += sizeof bytes) {
        assert(read_once(bytes, sizeof bytes, 1000) == (int)sizeof bytes && delays == 0);
        assert(memcmp(bytes, block + offset, sizeof bytes) == 0);
    }
    assert(esp_base_mqtt_ota_transfer_written(NULL, 2048, 4096));
    frame = request();
    assert(frame.offset == 2048 && frame.written_offset == 2048);
    assert(read_once(bytes, sizeof bytes, 1000) == -2 && delays == 1);

    /* Even a wakeup beyond the callback budget must not consume a newly
     * arrived block before returning retry. The OTA core rejects that late
     * callback; this direct adapter check inspects the still-owned bytes. */
    begin(2048, 30000);
    memset(bytes, 0xa5, sizeof bytes);
    offer_during_delay = true;
    wakeup_delay_us = 1000001;
    assert(read_once(bytes, sizeof bytes, 1000) == -2 && delays == 1);
    assert(now_us == 1010001);
    for (size_t i = 0; i < sizeof bytes; ++i) assert(bytes[i] == 0xa5);
    for (unsigned offset = 0; offset < sizeof block; offset += sizeof bytes) {
        assert(read_once(bytes, sizeof bytes, 1000) == (int)sizeof bytes && delays == 0);
        assert(memcmp(bytes, block + offset, sizeof bytes) == 0);
    }
}
static void test_read_boundaries(void)
{
    uint8_t byte = 0xa5;
    begin(2048, 30000);
    assert(read_once(NULL, 1, 1000) == -1 && delays == 0);
    assert(read_once(&byte, 0, 1000) == -1 && delays == 0);
    assert(read_once(&byte, 1, 0) == -1 && delays == 0);
    for (uint32_t budget = 1; budget <= portTICK_PERIOD_MS; ++budget)
        assert(read_once(&byte, 1, budget) == -2 && delays == 0 && now_us == 0);
    assert(read_once(&byte, 1, portTICK_PERIOD_MS + 1) == -2 && delays == 1 && now_us == 10000);
    now_us = -1;
    assert(read_once(&byte, 1, 1000) == -1 && delays == 0);
    now_us = INT64_MAX;
    assert(read_once(&byte, 1, 1000) == -1 && delays == 0);
    begin(2048, 30000);
    clock_values[0] = 100; clock_values[1] = 99; clock_count = 2;
    assert(read_once(&byte, 1, 1000) == -1 && delays == 0);
    begin(2048, 30000);
    clock_values[0] = 100; clock_values[1] = 101;
    clock_values[2] = 102; clock_values[3] = 100; clock_count = 4;
    assert(read_once(&byte, 1, 1000) == -1 && delays == 0);
    begin(2048, 30000);
    clock_values[0] = 0; clock_values[1] = 1000000; clock_count = 2;
    assert(read_once(&byte, 1, 1000) == -2 && delays == 0);
    begin(2048, 30000);
    wakeup_delay_us = -10001;
    assert(read_once(&byte, 1, 1000) == -1 && delays == 1);
    begin(2048, 30);
    for (unsigned i = 0; i < 3; ++i)
        assert(read_once(&byte, 1, 1000) == -2 && delays == 1);
    assert(now_us == 30000 && read_once(&byte, 1, 1000) == -1 && delays == 0);
    begin(2048, 30);
    now_us = 30000;
    assert(read_once(&byte, 1, 1000) == -1 && delays == 0);
}
static void test_immediate_terminal(void)
{
    uint8_t bytes[2048];
    begin(2048, 30000);
    ebase_mqtt_ota_frame_t frame = request();
    frame.kind = EBASE_MQTT_OTA_DATA; frame.data = block; frame.length = sizeof block;
    assert(esp_base_mqtt_ota_transfer_offer(&frame) == EBASE_MQTT_OTA_ACCEPTED);
    assert(read_once(bytes, sizeof bytes, 1000) == 2048 && delays == 0);
    assert(esp_base_mqtt_ota_transfer_written(NULL, 2048, 2048));
    frame = request(); frame.kind = EBASE_MQTT_OTA_END;
    assert(esp_base_mqtt_ota_transfer_offer(&frame) == EBASE_MQTT_OTA_ACCEPTED);
    assert(read_once(bytes, sizeof bytes, 1000) == 0 && delays == 0);
    begin(2048, 30000);
    frame = request(); frame.kind = EBASE_MQTT_OTA_CANCEL;
    assert(esp_base_mqtt_ota_transfer_offer(&frame) == EBASE_MQTT_OTA_ACCEPTED);
    assert(read_once(bytes, sizeof bytes, 1000) == -1 && delays == 0);
    esp_base_mqtt_ota_transfer_close();
}
int main(void)
{
    for (size_t i = 0; i < sizeof block; ++i) block[i] = (uint8_t)i;
    test_cross_call_wait();
    test_read_boundaries();
    test_immediate_terminal();
    puts("MQTT OTA: one-tick retry, cross-call payload and deadline boundaries passed (fake scheduler)");
}
