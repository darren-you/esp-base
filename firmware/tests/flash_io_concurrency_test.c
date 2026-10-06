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

int64_t esp_timer_get_time(void)
{
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
    assert(!ota_flash_release(&s_flash_io_owner));

    assert(pthread_create(&worker, NULL, contending_io, NULL) == 0);
    assert(pthread_create(&control, NULL, contending_io, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_join(control, NULL) == 0);
    assert(atomic_load(&completed_io) == 8192U);
    assert(atomic_load(&s_flash_io_owner.active_token) == 0U);
    assert(s_ota_flash_claim.owner == NULL && s_ota_flash_claim.token == 0U);

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
    puts("  production Flash callbacks passed (concurrent handoff, 8192 I/O claims, BUSY deadline)");
}
