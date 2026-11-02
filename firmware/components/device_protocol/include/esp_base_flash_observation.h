// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ESP_BASE_FLASH_OTA,
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

/* Stage observer uses only bounded integer copies under a dedicated short lock.
 * Bind/unbind and emit are called without a Flash or MQTT API lock. */
void esp_base_flash_phase_bind(const char *boot_id, const char *operation_id,
    const char *request_id, bool recovery);
void esp_base_flash_phase_unbind(void);
void esp_base_flash_phase_observe(void *context, uint32_t stage, bool active);
void esp_base_flash_phase_emit(const char *boot_id);
/* Existing 5-second report schedules changed rows; each control pass emits
 * at most one. Synchronous emit above is reserved for terminal/reboot flush. */
void esp_base_flash_phase_report_request(void);
void esp_base_flash_phase_poll(const char *boot_id);
/* Only the existing outer OTA worker changes this scope between SDK calls.
 * Source/registration verification cannot qualify prepare/select verification. */
typedef enum {
    ESP_BASE_FLASH_SCOPE_OTHER,
    ESP_BASE_FLASH_SCOPE_PREPARE,
    ESP_BASE_FLASH_SCOPE_SELECT,
} esp_base_flash_scope_t;
void esp_base_flash_phase_scope(esp_base_flash_scope_t scope);
