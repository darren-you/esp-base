// SPDX-License-Identifier: Apache-2.0
#include "esp_base_capacity.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#ifndef ESP_BASE_CAPACITY_SDK_LOCK_SHA256
#error "Capacity facts require the exact managed SDK lock digest"
#endif
#if CONFIG_FREERTOS_SMP || CONFIG_HEAP_TLSF_USE_ROM_IMPL || CONFIG_HEAP_TASK_TRACKING || !CONFIG_HEAP_POISONING_DISABLED
#error "Capacity facts require the managed non-ROM, non-SMP allocator with no owner/poisoning overhead"
#endif

enum { CAPACITY_TASK_LIMIT = 32, CAPACITY_PERIOD_MS = 5000, CAPACITY_DOMAIN_COUNT = 5 };
static TaskStatus_t s_tasks[CAPACITY_TASK_LIMIT];
static char s_task_names[CAPACITY_TASK_LIMIT][configMAX_TASK_NAME_LEN];
static uint64_t s_next_ms;
static uint32_t s_frame;
static bool s_frame_overflow;
static const uint32_t s_masks[CAPACITY_DOMAIN_COUNT] = {
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA,
    MALLOC_CAP_DEFAULT,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT,
};

typedef struct {
    const char *boot_id;
    uint32_t frame;
    size_t region_count;
    size_t minimum_free_sum[CAPACITY_DOMAIN_COUNT];
    size_t largest_lower_bound[CAPACITY_DOMAIN_COUNT];
    bool valid;
} capacity_frame_t;

static bool emit_region(const heap_capacity_region_stats_t *region, void *opaque)
{
    capacity_frame_t *frame = opaque;
    const multi_heap_capacity_stats_t *stats = &region->allocator;
    uint32_t caps = 0;
    for (unsigned i = 0; i < HEAP_CAPACITY_PRIORITIES; ++i) caps |= region->caps[i];
    ++frame->region_count;
    for (unsigned i = 0; i < CAPACITY_DOMAIN_COUNT; ++i) {
        if ((caps & s_masks[i]) != s_masks[i] || !region->available_at_heap_init) continue;
        if (SIZE_MAX - frame->minimum_free_sum[i] < stats->minimum_free_bytes) {
            frame->valid = false;
            continue;
        }
        frame->minimum_free_sum[i] += stats->minimum_free_bytes;
        if (stats->minimum_largest_allocatable_bytes > frame->largest_lower_bound[i])
            frame->largest_lower_bound[i] = stats->minimum_largest_allocatable_bytes;
    }
    esp_rom_printf("ESP_BASE_CAPACITY_REGION boot_id=%s frame=%u start=%08x end=%08x caps0=%08x caps1=%08x caps2=%08x alias_start=%08x alias_end=%08x alias_inverted=%u available_at_heap_init=%u free_bytes=%u min_free_bytes=%u largest_request_bytes=%u min_largest_request_bytes=%u allocator_metadata_bytes=%u\n",
        frame->boot_id, (unsigned)frame->frame, (unsigned)region->start, (unsigned)region->end,
        (unsigned)region->caps[0], (unsigned)region->caps[1], (unsigned)region->caps[2],
        (unsigned)region->alias_start, (unsigned)region->alias_end, (unsigned)region->alias_inverted,
        (unsigned)region->available_at_heap_init, (unsigned)stats->current_free_bytes,
        (unsigned)stats->minimum_free_bytes, (unsigned)stats->current_largest_allocatable_bytes,
        (unsigned)stats->minimum_largest_allocatable_bytes, (unsigned)stats->allocator_metadata_bytes);
    return true;
}

static void emit(const char *boot_id, const char *phase, uint64_t now_ms)
{
    if (s_frame == UINT32_MAX) s_frame_overflow = true;
    else ++s_frame;
    capacity_frame_t frame = {.boot_id = boot_id, .frame = s_frame, .valid = !s_frame_overflow};
    esp_rom_printf("ESP_BASE_CAPACITY_BEGIN schema=1 boot_id=%s frame=%u phase=%s uptime_ms=%llu sdk_lock_sha256=%s task_limit=%u\n",
        boot_id, (unsigned)s_frame, phase, (unsigned long long)now_ms,
        ESP_BASE_CAPACITY_SDK_LOCK_SHA256, CAPACITY_TASK_LIMIT);
    heap_caps_walk_capacity(emit_region, &frame);

    TaskCapacityStats_t task_stats;
    /* SystemState holds pointers to TCB names. Copy them before the outer resume can finalize a TCB. */
    vTaskSuspendAll();
    const UBaseType_t count = uxTaskGetCapacitySnapshot(s_tasks, CAPACITY_TASK_LIMIT, &task_stats);
    for (UBaseType_t i = 0; i < count && i < CAPACITY_TASK_LIMIT; ++i)
        memcpy(s_task_names[i], s_tasks[i].pcTaskName, configMAX_TASK_NAME_LEN);
    (void)xTaskResumeAll();
    const bool complete = count > 0 && count <= CAPACITY_TASK_LIMIT && task_stats.counters_valid &&
        task_stats.finalized_instances <= task_stats.created_instances &&
        task_stats.created_instances - task_stats.finalized_instances == count;
    size_t minimum_stack = SIZE_MAX;
    if (task_stats.finalized_instances > 0)
        minimum_stack = (size_t)task_stats.completed_minimum * sizeof(StackType_t);
    for (UBaseType_t i = 0; i < count && i < CAPACITY_TASK_LIMIT; ++i) {
        const size_t stack = (size_t)s_tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        if (stack < minimum_stack) minimum_stack = stack;
        char name_hex[configMAX_TASK_NAME_LEN * 2 + 1];
        static const char hex[] = "0123456789abcdef";
        for (unsigned j = 0; j < configMAX_TASK_NAME_LEN; ++j) {
            const unsigned char ch = (unsigned char)s_task_names[i][j];
            name_hex[j * 2] = hex[ch >> 4]; name_hex[j * 2 + 1] = hex[ch & 15];
        }
        name_hex[configMAX_TASK_NAME_LEN * 2] = 0;
        esp_rom_printf("ESP_BASE_CAPACITY_TASK boot_id=%s frame=%u instance=%u name_hex=%s minimum_stack_bytes=%u state=%u\n",
            boot_id, (unsigned)s_frame, (unsigned)s_tasks[i].xTaskNumber, name_hex, (unsigned)stack,
            (unsigned)s_tasks[i].eCurrentState);
    }
    for (unsigned i = 0; i < CAPACITY_DOMAIN_COUNT; ++i)
        esp_rom_printf("ESP_BASE_CAPACITY_DOMAIN boot_id=%s frame=%u caps=%08x alignment_bytes=4 minimum_free_lower_bound_bytes=%u largest_request_lower_bound_bytes=%u history=region_minima_conservative_bound\n",
            boot_id, (unsigned)s_frame, (unsigned)s_masks[i],
            (unsigned)frame.minimum_free_sum[i], (unsigned)frame.largest_lower_bound[i]);
    esp_rom_printf("ESP_BASE_CAPACITY_END boot_id=%s frame=%u regions=%u allocated_tasks=%u created_instances=%u finalized_instances=%u completed_stack_min_bytes=%u worst_completed_instance=%u minimum_stack_bytes=%u task_snapshot_complete=%u counters_valid=%u facts_valid=%u workspace_bytes=%u observation_cost_added_back=0\n",
        boot_id, (unsigned)s_frame, (unsigned)frame.region_count, (unsigned)count,
        (unsigned)task_stats.created_instances, (unsigned)task_stats.finalized_instances,
        task_stats.finalized_instances ? (unsigned)((size_t)task_stats.completed_minimum * sizeof(StackType_t)) : 0U,
        (unsigned)task_stats.worst_completed_instance, (unsigned)(minimum_stack == SIZE_MAX ? 0 : minimum_stack),
        (unsigned)complete, (unsigned)task_stats.counters_valid, (unsigned)frame.valid,
        (unsigned)(sizeof s_tasks + sizeof s_task_names + sizeof s_next_ms + sizeof s_frame + sizeof s_frame_overflow));
}

void esp_base_capacity_poll(const char *boot_id, uint64_t now_ms)
{
    if (now_ms < s_next_ms) return;
    s_next_ms = now_ms <= UINT64_MAX - CAPACITY_PERIOD_MS ? now_ms + CAPACITY_PERIOD_MS : UINT64_MAX;
    emit(boot_id, "periodic", now_ms);
}

void esp_base_capacity_before_reset(const char *boot_id, uint64_t now_ms)
{
    emit(boot_id, "before_reset", now_ms);
}
