// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "../components/device_protocol/flash_observation.c"

int64_t esp_timer_get_time(void) { return 1000; }

/* Only this isolated fixture seeds boundary values; production never resets
 * its boot history or exposes a mutation API. */
static void reset_fixture(void)
{
    memset(s_metrics, 0, sizeof s_metrics);
    for (unsigned i = 0; i < ESP_BASE_FLASH_CONSUMER_COUNT; ++i)
        s_metrics[i].counters_valid = 1U;
}

static atomic_uint finished_workers;
static void *worker(void *context)
{
    const esp_base_flash_consumer_t consumer = (esp_base_flash_consumer_t)(uintptr_t)context;
    for (unsigned i = 0; i < 4096U; ++i) {
        esp_base_flash_observation_acquire(consumer, 100, 104, true);
        esp_base_flash_observation_release(consumer, 101, 110, true);
    }
    atomic_fetch_add(&finished_workers, 1U);
    return NULL;
}

int main(void)
{
    reset_fixture();
    pthread_t workers[ESP_BASE_FLASH_CONSUMER_COUNT];
    for (unsigned i = 0; i < ESP_BASE_FLASH_CONSUMER_COUNT; ++i)
        assert(pthread_create(&workers[i], NULL, worker, (void *)(uintptr_t)i) == 0);
    esp_base_flash_observation_t snapshot[ESP_BASE_FLASH_CONSUMER_COUNT];
    while (atomic_load(&finished_workers) != ESP_BASE_FLASH_CONSUMER_COUNT) {
        esp_base_flash_observation_snapshot(snapshot);
        for (unsigned i = 0; i < ESP_BASE_FLASH_CONSUMER_COUNT; ++i) {
            assert(snapshot[i].completed_claim_count <= 4096U && snapshot[i].counters_valid);
            if (snapshot[i].completed_claim_count) {
                assert(snapshot[i].max_wait_us == 4U);
                assert(snapshot[i].max_held_upper_bound_us == 9U);
            }
        }
    }
    for (unsigned i = 0; i < ESP_BASE_FLASH_CONSUMER_COUNT; ++i)
        assert(pthread_join(workers[i], NULL) == 0);
    esp_base_flash_observation_snapshot(snapshot);
    for (unsigned i = 0; i < ESP_BASE_FLASH_CONSUMER_COUNT; ++i)
        assert(snapshot[i].completed_claim_count == 4096U && snapshot[i].counters_valid);

    esp_base_flash_observation_acquire(ESP_BASE_FLASH_CONFIG, 100, 107, false);
    esp_base_flash_observation_release(ESP_BASE_FLASH_CONFIG, 0, 1000, false);
    esp_base_flash_observation_snapshot(snapshot);
    assert(snapshot[ESP_BASE_FLASH_CONFIG].acquire_failed_count == 1U);
    assert(snapshot[ESP_BASE_FLASH_CONFIG].release_failed_count == 1U);
    assert(snapshot[ESP_BASE_FLASH_CONFIG].max_wait_us == 7U);
    assert(snapshot[ESP_BASE_FLASH_CONFIG].max_held_upper_bound_us == 9U);
    assert(snapshot[ESP_BASE_FLASH_CONFIG].completed_claim_count == 4096U);
    assert(!snapshot[ESP_BASE_FLASH_CONFIG].counters_valid);

    reset_fixture();
    s_metrics[ESP_BASE_FLASH_OTA].completed_claim_count = UINT32_MAX - 1U;
    esp_base_flash_observation_release(ESP_BASE_FLASH_OTA, 1, 2, true);
    assert(s_metrics[ESP_BASE_FLASH_OTA].counters_valid);
    esp_base_flash_observation_release(ESP_BASE_FLASH_OTA, 1, 2, true);
    assert(s_metrics[ESP_BASE_FLASH_OTA].completed_claim_count == UINT32_MAX);
    assert(!s_metrics[ESP_BASE_FLASH_OTA].counters_valid);
    s_metrics[ESP_BASE_FLASH_FRP].acquire_failed_count = UINT32_MAX;
    esp_base_flash_observation_acquire(ESP_BASE_FLASH_FRP, 1, 2, false);
    assert(s_metrics[ESP_BASE_FLASH_FRP].acquire_failed_count == UINT32_MAX);
    assert(!s_metrics[ESP_BASE_FLASH_FRP].counters_valid);

    reset_fixture();
    esp_base_flash_observation_acquire(ESP_BASE_FLASH_OTA, 0, UINT32_MAX, true);
    assert(s_metrics[ESP_BASE_FLASH_OTA].max_wait_us == UINT32_MAX);
    assert(s_metrics[ESP_BASE_FLASH_OTA].counters_valid);
    esp_base_flash_observation_acquire(ESP_BASE_FLASH_OTA, 0, (int64_t)UINT32_MAX + 1, true);
    assert(s_metrics[ESP_BASE_FLASH_OTA].max_wait_us == UINT32_MAX);
    assert(!s_metrics[ESP_BASE_FLASH_OTA].counters_valid);
    esp_base_flash_observation_release(ESP_BASE_FLASH_FRP, 0, INT64_MAX, true);
    assert(s_metrics[ESP_BASE_FLASH_FRP].max_held_upper_bound_us == UINT32_MAX);
    assert(!s_metrics[ESP_BASE_FLASH_FRP].counters_valid);
    esp_base_flash_observation_acquire(ESP_BASE_FLASH_CONFIG, INT64_MIN, INT64_MAX, false);
    assert(!s_metrics[ESP_BASE_FLASH_CONFIG].counters_valid);
    reset_fixture();
    esp_base_flash_observation_release(ESP_BASE_FLASH_CONFIG, 10, 9, true);
    esp_base_flash_observation_release(ESP_BASE_FLASH_CONFIG, 10, 11, true);
    assert(!s_metrics[ESP_BASE_FLASH_CONFIG].counters_valid);
    assert(s_metrics[ESP_BASE_FLASH_CONFIG].max_held_upper_bound_us == 1U);
    esp_base_flash_observation_snapshot(NULL);
    esp_base_flash_observation_acquire(ESP_BASE_FLASH_CONSUMER_COUNT, 0, 1, false);
    esp_base_flash_observation_release((esp_base_flash_consumer_t)-1, 0, 1, true);

    FILE *capture = tmpfile();
    assert(capture);
    fflush(stdout);
    const int original_stdout = dup(STDOUT_FILENO);
    assert(original_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    esp_base_flash_observation_emit("11111111-1111-4111-8111-111111111111");
    fflush(stdout);
    assert(dup2(original_stdout, STDOUT_FILENO) >= 0);
    close(original_stdout);
    rewind(capture);
    char output[4096] = {0};
    assert(fread(output, 1U, sizeof output - 1U, capture) > 0);
    fclose(capture);
    assert(strstr(output, "ESP_BASE_FLASH_IO boot_id="));
    assert(strstr(output, "consumer=ota") && strstr(output, "consumer=frp") && strstr(output, "consumer=config"));
    assert(strstr(output, "ESP_BASE_FLASH_IO_OUTPUT") && strstr(output, "snapshot_copy_bytes=72"));
    assert(!strstr(output, "ESP_BASE_CAPACITY_"));
    puts("  Flash observation passed (concurrent snapshots, saturating counters/time, invalid clock/release, independent UART)");
}
