// SPDX-License-Identifier: Apache-2.0
#include "esp_base_flash_observation.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_base_flash_observation_t s_metrics[ESP_BASE_FLASH_CONSUMER_COUNT] = {
    {.counters_valid = 1U}, {.counters_valid = 1U}, {.counters_valid = 1U}
};
_Static_assert(sizeof(esp_base_flash_observation_t) == 6U * sizeof(uint32_t),
    "Flash observations must retain fixed 32-bit fields without hidden padding");

static bool valid_consumer(esp_base_flash_consumer_t consumer)
{
    return consumer >= ESP_BASE_FLASH_OTA && consumer < ESP_BASE_FLASH_CONSUMER_COUNT;
}

static void increment(esp_base_flash_observation_t *metrics, uint32_t *counter)
{
    if (*counter == UINT32_MAX) metrics->counters_valid = 0U;
    else ++*counter;
}

static void duration_max(esp_base_flash_observation_t *metrics, uint32_t *maximum,
    int64_t started_us, int64_t finished_us)
{
    if (started_us < 0 || finished_us < started_us ||
        (uint64_t)finished_us - (uint64_t)started_us > UINT32_MAX) {
        metrics->counters_valid = 0U;
        if (started_us >= 0 && finished_us >= started_us) *maximum = UINT32_MAX;
        return;
    }
    const uint32_t elapsed = (uint32_t)((uint64_t)finished_us - (uint64_t)started_us);
    if (elapsed > *maximum) *maximum = elapsed;
}

void esp_base_flash_observation_acquire(esp_base_flash_consumer_t consumer,
    int64_t started_us, int64_t finished_us, bool acquired)
{
    if (!valid_consumer(consumer)) return;
    portENTER_CRITICAL(&s_metrics_lock);
    esp_base_flash_observation_t *metrics = &s_metrics[consumer];
    duration_max(metrics, &metrics->max_wait_us, started_us, finished_us);
    if (!acquired) increment(metrics, &metrics->acquire_failed_count);
    portEXIT_CRITICAL(&s_metrics_lock);
}

void esp_base_flash_observation_release(esp_base_flash_consumer_t consumer,
    int64_t held_started_us, int64_t finished_us, bool released)
{
    if (!valid_consumer(consumer)) return;
    portENTER_CRITICAL(&s_metrics_lock);
    esp_base_flash_observation_t *metrics = &s_metrics[consumer];
    if (released) {
        duration_max(metrics, &metrics->max_held_upper_bound_us, held_started_us, finished_us);
        increment(metrics, &metrics->completed_claim_count);
    }
    else {
        increment(metrics, &metrics->release_failed_count);
        metrics->counters_valid = 0U;
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

void esp_base_flash_observation_snapshot(
    esp_base_flash_observation_t out[ESP_BASE_FLASH_CONSUMER_COUNT])
{
    if (!out) return;
    portENTER_CRITICAL(&s_metrics_lock);
    memcpy(out, s_metrics, sizeof s_metrics);
    portEXIT_CRITICAL(&s_metrics_lock);
}

void esp_base_flash_observation_emit(const char *boot_id)
{
    static const char *const names[] = {"ota", "frp", "config"};
    esp_base_flash_observation_t metrics[ESP_BASE_FLASH_CONSUMER_COUNT];
    const int64_t started_us = esp_timer_get_time();
    esp_base_flash_observation_snapshot(metrics);
    const int64_t copied_us = esp_timer_get_time();
    flockfile(stdout);
    for (unsigned i = 0; i < ESP_BASE_FLASH_CONSUMER_COUNT; ++i) {
        printf("ESP_BASE_FLASH_IO boot_id=%s snapshot_started_us=%" PRId64 " snapshot_finished_us=%" PRId64 " consumer=%s completed_claim_count=%" PRIu32 " acquire_failed_count=%" PRIu32 " release_failed_count=%" PRIu32 " max_wait_us=%" PRIu32 " max_held_upper_bound_us=%" PRIu32 " counters_valid=%" PRIu32 " aggregate_storage_bytes=%u snapshot_copy_bytes=%u\n",
            boot_id, started_us, copied_us, names[i], metrics[i].completed_claim_count,
            metrics[i].acquire_failed_count, metrics[i].release_failed_count,
            metrics[i].max_wait_us, metrics[i].max_held_upper_bound_us,
            metrics[i].counters_valid, (unsigned)(sizeof s_metrics + sizeof s_metrics_lock),
            (unsigned)sizeof metrics);
    }
    funlockfile(stdout);
    const int64_t output_finished_us = esp_timer_get_time();
    printf("ESP_BASE_FLASH_IO_OUTPUT boot_id=%s started_us=%" PRId64 " finished_us=%" PRId64 " scope=snapshot_and_three_rows_excluding_this_line\n",
        boot_id, started_us, output_finished_us);
}
