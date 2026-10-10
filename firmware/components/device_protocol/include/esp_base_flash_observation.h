// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ESP_BASE_FLASH_OTA,
    ESP_BASE_FLASH_FRP,
    ESP_BASE_FLASH_CONFIG,
    ESP_BASE_FLASH_CONSUMER_COUNT
} esp_base_flash_consumer_t;

typedef struct {
    uint32_t completed_claim_count;
    uint32_t acquire_failed_count;
    uint32_t release_failed_count;
    uint32_t max_wait_us;
    uint32_t max_held_upper_bound_us;
    uint32_t counters_valid;
} esp_base_flash_observation_t;

/* Clocks and Flash owner operations are outside the metrics critical section.
 * held starts before the successful CAS and ends after the successful release;
 * its measured maximum is an upper bound for completed calls, not an operation
 * bound or a simultaneous sum across consumers. Failed release invalidates the
 * boot history. Counters and durations saturate without resetting history. */
void esp_base_flash_observation_acquire(esp_base_flash_consumer_t consumer,
    int64_t started_us, int64_t finished_us, bool acquired);
void esp_base_flash_observation_release(esp_base_flash_consumer_t consumer,
    int64_t held_started_us, int64_t finished_us, bool released);
void esp_base_flash_observation_snapshot(
    esp_base_flash_observation_t out[ESP_BASE_FLASH_CONSUMER_COUNT]);
/* Called after capacity END in the existing control report; creates no task,
 * timer, allocation or independent reporting period. */
void esp_base_flash_observation_emit(const char *boot_id);
