// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_ota_transfer.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { IMAGE_SIZE = 40 * 1024 + 19 };
static atomic_bool finished;
static uint32_t written_count;

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = {.tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000};
    nanosleep(&delay, NULL);
}

static void *worker(void *unused)
{
    (void)unused;
    uint8_t bytes[64];
    uint32_t read = 0;
    while (read < IMAGE_SIZE) {
        const int count = esp_base_mqtt_ota_transfer_read(NULL, bytes, sizeof bytes, 100);
        if (count == -2) continue;
        assert(count > 0 && count <= 64 && read + (uint32_t)count <= IMAGE_SIZE);
        for (int i = 0; i < count; ++i) assert(bytes[i] == (uint8_t)(read + (uint32_t)i));
        read += (uint32_t)count;
        if (read % 1024 == 0) {
            /* Model a delayed sequential Flash write. No mailbox lock is
             * held during the delay; requests must not acknowledge it yet. */
            vTaskDelay(1);
            assert(esp_base_mqtt_ota_transfer_written(NULL, read, IMAGE_SIZE));
            written_count = read;
        }
    }
    int end;
    do { end = esp_base_mqtt_ota_transfer_read(NULL, bytes, 1, 100); } while (end == -2);
    assert(end == 0);
    assert(esp_base_mqtt_ota_transfer_written(NULL, IMAGE_SIZE, IMAGE_SIZE));
    written_count = IMAGE_SIZE;
    atomic_store_explicit(&finished, true, memory_order_release);
    return NULL;
}

int main(void)
{
    assert(esp_base_mqtt_ota_transfer_begin(IMAGE_SIZE, 10000, 2000));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, worker, NULL) == 0);
    uint8_t block[1024];
    unsigned delivered = 0;
    const int64_t deadline = esp_timer_get_time() + 10000000;
    while (!atomic_load_explicit(&finished, memory_order_acquire)) {
        assert(esp_timer_get_time() < deadline);
        ebase_mqtt_ota_frame_t request;
        if (!esp_base_mqtt_ota_transfer_request(&request)) { vTaskDelay(1); continue; }
        assert(request.offset <= IMAGE_SIZE && request.written_offset <= request.offset);
        if (request.offset < IMAGE_SIZE) assert(request.written_offset == request.offset);
        else assert(request.written_offset == 40 * 1024);
        for (size_t i = 0; i < sizeof block; ++i) block[i] = (uint8_t)(request.offset + i);
        request.kind = request.offset == IMAGE_SIZE ? EBASE_MQTT_OTA_END : EBASE_MQTT_OTA_DATA;
        request.data = request.kind == EBASE_MQTT_OTA_DATA ? block : NULL;
        request.length = IMAGE_SIZE - request.offset;
        if (request.length > sizeof block) request.length = sizeof block;
        request.tag[0] = (uint8_t)(request.offset / 1024 + 1);
        assert(esp_base_mqtt_ota_transfer_offer(&request) == EBASE_MQTT_OTA_ACCEPTED);
        ++delivered;
        assert(esp_base_mqtt_ota_transfer_offer(&request) == EBASE_MQTT_OTA_DUPLICATE);
        memset(block, 0xa5, sizeof block);
    }
    assert(pthread_join(thread, NULL) == 0 && written_count == IMAGE_SIZE && delivered == 42);
    assert(esp_base_mqtt_ota_transfer_select());
    assert(!esp_base_mqtt_ota_transfer_select());
    esp_base_mqtt_ota_transfer_close();
    assert(esp_base_mqtt_ota_transfer_begin(1024, 1000, 100));
    uint8_t byte;
    assert(esp_base_mqtt_ota_transfer_read(NULL, &byte, 1, 10) == -2);
    ebase_mqtt_ota_frame_t request;
    assert(esp_base_mqtt_ota_transfer_request(&request));
    request.kind = EBASE_MQTT_OTA_CANCEL;
    assert(esp_base_mqtt_ota_transfer_offer(&request) == EBASE_MQTT_OTA_ACCEPTED);
    assert(esp_base_mqtt_ota_transfer_read(NULL, &byte, 1, 10) == -1);
    assert(!esp_base_mqtt_ota_transfer_select());
    esp_base_mqtt_ota_transfer_close();
    puts("MQTT OTA: concurrent control/worker mailbox passed (SDK/Flash fakes, no hardware)");
}
