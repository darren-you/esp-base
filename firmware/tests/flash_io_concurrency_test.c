// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

/* Exercise the production callbacks, rather than a mutex-protected substitute.
 * Unused startup functions are removed by the host linker's section GC. */
#include "../apps/esp_base/main/esp_base_main.c"

static _Thread_local const int64_t *scripted_clock;
static _Thread_local unsigned scripted_clock_index;

int64_t esp_timer_get_time(void)
{
    if (scripted_clock) return scripted_clock[scripted_clock_index++];
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (int64_t)value.tv_sec * INT64_C(1000000) + value.tv_nsec / 1000;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    sched_yield();
}

static atomic_bool holder_ready, competitor_started, holder_may_release;
static atomic_uint simultaneous_io, completed_io;

static void *holder(void *unused)
{
    (void)unused;
    assert(ota_flash_acquire(&s_flash_io_owner));
    atomic_store(&holder_ready, true);
    while (!atomic_load(&holder_may_release)) sched_yield();
    assert(esp_base_storage_claim_active(&s_ota_flash_claim));
    assert(ota_flash_release(&s_flash_io_owner));
    return NULL;
}

static void *competitor(void *unused)
{
    (void)unused;
    while (!atomic_load(&holder_ready)) sched_yield();
    atomic_store(&competitor_started, true);
    assert(ota_flash_acquire(&s_flash_io_owner));
    assert(esp_base_storage_claim_active(&s_ota_flash_claim));
    assert(ota_flash_release(&s_flash_io_owner));
    return NULL;
}

static void *contending_io(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 4096U; ++i) {
        assert(ota_flash_acquire(&s_flash_io_owner));
        assert(atomic_fetch_add(&simultaneous_io, 1U) == 0U);
        assert(esp_base_storage_claim_active(&s_ota_flash_claim));
        sched_yield();
        assert(atomic_fetch_sub(&simultaneous_io, 1U) == 1U);
        assert(ota_flash_release(&s_flash_io_owner));
        atomic_fetch_add(&completed_io, 1U);
    }
    return NULL;
}

int main(void)
{
    esp_base_storage_owner_init(&s_flash_io_owner);
    assert(!ota_flash_acquire(NULL));
    assert(!ota_flash_release(NULL));
    pthread_t worker, control;
    assert(pthread_create(&worker, NULL, holder, NULL) == 0);
    assert(pthread_create(&control, NULL, competitor, NULL) == 0);
    while (!atomic_load(&competitor_started)) sched_yield();
    struct timespec pause = {.tv_nsec = 10000000};
    assert(nanosleep(&pause, NULL) == 0);
    atomic_store(&holder_may_release, true);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_join(control, NULL) == 0);
    assert(pthread_create(&worker, NULL, contending_io, NULL) == 0);
    assert(pthread_create(&control, NULL, contending_io, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_join(control, NULL) == 0);
    assert(atomic_load(&completed_io) == 8192U);
    assert(atomic_load(&s_flash_io_owner.active_token) == 0U);
    assert(s_ota_flash_claim.owner == NULL && s_ota_flash_claim.token == 0U);
    assert(s_ota_flash_held_started_us == 0);
    esp_base_flash_observation_t metrics[ESP_BASE_FLASH_CONSUMER_COUNT];
    esp_base_flash_observation_snapshot(metrics);
    assert(metrics[ESP_BASE_FLASH_OTA].completed_claim_count == 8194U);
    assert(metrics[ESP_BASE_FLASH_OTA].counters_valid);
    assert(metrics[ESP_BASE_FLASH_OTA].max_wait_us > 0U &&
           metrics[ESP_BASE_FLASH_OTA].max_held_upper_bound_us > 0U);

    /* BUSY still expires after the existing 500 ms wait and cannot consume or
     * clear the holder's claim. This test calls acquire again on the holder. */
    assert(ota_flash_acquire(&s_flash_io_owner));
    const unsigned token = s_ota_flash_claim.token;
    const int64_t started_us = esp_timer_get_time();
    assert(!ota_flash_acquire(&s_flash_io_owner));
    assert(esp_timer_get_time() - started_us >= ESP_BASE_FLASH_IO_WAIT_US);
    assert(s_ota_flash_claim.token == token);
    assert(esp_base_storage_claim_active(&s_ota_flash_claim));
    assert(ota_flash_release(&s_flash_io_owner));
    esp_base_flash_observation_snapshot(metrics);
    assert(metrics[ESP_BASE_FLASH_OTA].acquire_failed_count == 1U);
    assert(metrics[ESP_BASE_FLASH_OTA].max_wait_us >= ESP_BASE_FLASH_IO_WAIT_US);
    assert(metrics[ESP_BASE_FLASH_OTA].completed_claim_count == 8195U);
    assert(metrics[ESP_BASE_FLASH_OTA].counters_valid);

    /* An invalid owner token makes the real release fail. Restore the fixture
     * token only after verifying that both shared handoff values survived. */
    assert(ota_flash_acquire(&s_flash_io_owner));
    const esp_base_storage_claim_t held_claim = s_ota_flash_claim;
    const int64_t held_start = s_ota_flash_held_started_us;
    atomic_store(&s_flash_io_owner.active_token, held_claim.token + 1U);
    assert(!ota_flash_release(&s_flash_io_owner));
    assert(s_ota_flash_claim.owner == held_claim.owner &&
           s_ota_flash_claim.token == held_claim.token &&
           s_ota_flash_held_started_us == held_start);
    atomic_store(&s_flash_io_owner.active_token, held_claim.token);
    assert(ota_flash_release(&s_flash_io_owner));
    /* Releasing an empty handoff also remains a failure. */
    assert(!ota_flash_release(&s_flash_io_owner));
    assert(s_ota_flash_claim.owner == NULL && s_ota_flash_held_started_us == 0);
    esp_base_flash_observation_snapshot(metrics);
    assert(metrics[ESP_BASE_FLASH_OTA].release_failed_count == 2U &&
           !metrics[ESP_BASE_FLASH_OTA].counters_valid);

    assert(ota_flash_acquire(&s_flash_io_owner));
    const int64_t original_held_started_us = s_ota_flash_held_started_us;
    const unsigned original_token = s_ota_flash_claim.token;
    const int64_t reversed_clock[] = {100, 99, 101};
    scripted_clock = reversed_clock; scripted_clock_index = 0;
    assert(!ota_flash_acquire(&s_flash_io_owner));
    assert(scripted_clock_index == 3U && s_ota_flash_claim.token == original_token &&
           s_ota_flash_held_started_us == original_held_started_us);
    scripted_clock = NULL;
    assert(ota_flash_release(&s_flash_io_owner));

    const int64_t success_clock[] = {100, 101, 103, 109};
    scripted_clock = success_clock; scripted_clock_index = 0;
    assert(ota_flash_acquire(&s_flash_io_owner));
    assert(s_ota_flash_held_started_us == 101);
    assert(ota_flash_release(&s_flash_io_owner));
    assert(scripted_clock_index == 4U && s_ota_flash_held_started_us == 0);
    scripted_clock = NULL;
    puts("  production Flash callbacks passed (concurrent handoff, 8192 I/O claims, BUSY deadline)");
}
