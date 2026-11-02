// SPDX-License-Identifier: Apache-2.0
#include "esp_base_capacity.h"
#include "esp_base_mqtt_owner.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
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

enum { CAPACITY_TASK_LIMIT = 32, CAPACITY_REGION_LIMIT = 8, CAPACITY_PERIOD_MS = 5000, CAPACITY_DOMAIN_COUNT = 5 };
/* Heap rows are emitted before the task snapshot, so they share the existing
 * task workspace. The bounded copy never adds a second heap-sized buffer. */
static union {
    TaskStatus_t tasks[CAPACITY_TASK_LIMIT];
    heap_capacity_region_stats_t regions[CAPACITY_REGION_LIMIT];
} s_work;
_Static_assert(sizeof s_work.regions <= sizeof s_work.tasks, "region snapshots must reuse the task workspace");
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
} esp_base_capacity_frame_t;

static bool emit_region(const heap_capacity_region_stats_t *region, void *opaque)
{
    esp_base_capacity_frame_t *frame = opaque;
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
    printf("ESP_BASE_CAPACITY_REGION boot_id=%s frame=%u start=%08x end=%08x caps0=%08x caps1=%08x caps2=%08x alias_start=%08x alias_end=%08x alias_inverted=%u available_at_heap_init=%u free_bytes=%u min_free_bytes=%u largest_request_bytes=%u min_largest_request_bytes=%u allocator_metadata_bytes=%u\n",
        frame->boot_id, (unsigned)frame->frame, (unsigned)region->start, (unsigned)region->end,
        (unsigned)region->caps[0], (unsigned)region->caps[1], (unsigned)region->caps[2],
        (unsigned)region->alias_start, (unsigned)region->alias_end, (unsigned)region->alias_inverted,
        (unsigned)region->available_at_heap_init, (unsigned)stats->current_free_bytes,
        (unsigned)stats->minimum_free_bytes, (unsigned)stats->current_largest_allocatable_bytes,
        (unsigned)stats->minimum_largest_allocatable_bytes, (unsigned)stats->allocator_metadata_bytes);
    return true;
}

typedef enum { OUTPUT_BEGIN, OUTPUT_MQTT, OUTPUT_COLLECT_REGIONS, OUTPUT_REGION,
    OUTPUT_COLLECT_TASKS, OUTPUT_TASK, OUTPUT_DOMAIN, OUTPUT_END } output_stage_t;
static struct {
    esp_base_capacity_frame_t frame;
    emqtt_capacity_stats_t mqtt;
    freertos_task_capacity_stats_t task_stats;
    const char *phase;
    uint64_t begin_uptime_ms;
    size_t stored_regions, index, minimum_stack;
    UBaseType_t task_count;
    output_stage_t stage;
    bool active, mqtt_available, complete;
} s_output;

static bool collect_region(const heap_capacity_region_stats_t *region, void *unused)
{
    (void)unused;
    if (s_output.stored_regions == CAPACITY_REGION_LIMIT) {
        s_output.frame.valid = false;
        return false; /* Never qualify a truncated region inventory. */
    }
    s_work.regions[s_output.stored_regions++] = *region;
    return true;
}

static void start_frame(const char *boot_id, const char *phase, uint64_t now_ms)
{
    memset(&s_output, 0, sizeof s_output);
    s_output.mqtt_available = esp_base_mqtt_owner_capacity(&s_output.mqtt);
    /* The copied MQTT tuple must not postdate BEGIN. All heap/task getters
     * remain after BEGIN; their minima still qualify only its prefix. */
    const uint64_t copied_at_ms = (uint64_t)esp_timer_get_time() / 1000U;
    if (copied_at_ms > now_ms) now_ms = copied_at_ms;
    if (s_frame == UINT32_MAX) s_frame_overflow = true;
    else ++s_frame;
    s_output.frame = (esp_base_capacity_frame_t){.boot_id=boot_id, .frame=s_frame, .valid=!s_frame_overflow};
    s_output.phase = phase;
    s_output.begin_uptime_ms = now_ms;
    s_output.stage = OUTPUT_BEGIN;
    s_output.active = true;
}

static void collect_tasks(void)
{
    /* Copy names before resume can finalize a TCB. Scalar task rows and the
     * copied names stay valid while later control passes emit them. */
    vTaskSuspendAll();
    const UBaseType_t count = freertos_task_get_capacity_snapshot(s_work.tasks, CAPACITY_TASK_LIMIT, &s_output.task_stats);
    for (UBaseType_t i=0; i<count && i<CAPACITY_TASK_LIMIT; ++i)
        memcpy(s_task_names[i], s_work.tasks[i].pcTaskName, configMAX_TASK_NAME_LEN);
    (void)xTaskResumeAll();
    s_output.task_count = count;
    s_output.complete = count > 0 && count <= CAPACITY_TASK_LIMIT && s_output.task_stats.counters_valid &&
        s_output.task_stats.finalized_instances <= s_output.task_stats.created_instances &&
        s_output.task_stats.created_instances - s_output.task_stats.finalized_instances == count;
    s_output.minimum_stack = s_output.task_stats.finalized_instances ?
        (size_t)s_output.task_stats.completed_minimum * sizeof(StackType_t) : SIZE_MAX;
}

/* At most one output row per ordinary control pass. This bounds synchronous
 * UART work by one row and lets the existing owner drain MQTT between rows. */
static void emit_step(void)
{
    while (s_output.active) {
        switch (s_output.stage) {
        case OUTPUT_BEGIN: {
            uint64_t now_ms = s_output.begin_uptime_ms;
            char uptime[21];
            char *digits = uptime + sizeof uptime - 1;
            *digits = '\0';
            do { *--digits = (char)('0' + now_ms % 10U); now_ms /= 10U; } while (now_ms);
            printf("ESP_BASE_CAPACITY_BEGIN schema=2 boot_id=%s frame=%u phase=%s uptime_ms=%s sdk_lock_sha256=%s task_limit=%u\n",
                s_output.frame.boot_id, (unsigned)s_output.frame.frame, s_output.phase, digits,
                ESP_BASE_CAPACITY_SDK_LOCK_SHA256, CAPACITY_TASK_LIMIT);
            s_output.stage = OUTPUT_MQTT;
            return;
        }
        case OUTPUT_MQTT:
            printf("ESP_BASE_CAPACITY_MQTT boot_id=%s frame=%u available=%u runtime_instance=%u rx_peak_valid=%u rx_peak_uptime_ms=%llu rx_peak_observed_until_uptime_ms=%llu complete_owner_count=%u complete_payload_bytes=%u partial_declared_bytes=%u partial_received_bytes=%u owned_message_metadata_bytes=%u owned_request_bytes=%u outbox_wire_bytes_at_rx_peak=%u outbox_full_count=%u last_outbox_full_payload_bytes=%u last_outbox_full_uptime_ms=%llu notice_count_high_water=%u notice_full_count=%u counters_valid=%u observation_storage_bytes=%u readout_copy_bytes=%u\n",
                s_output.frame.boot_id, (unsigned)s_output.frame.frame, (unsigned)s_output.mqtt_available, (unsigned)s_output.mqtt.runtime_instance,
                (unsigned)s_output.mqtt.rx_peak_valid, (unsigned long long)s_output.mqtt.rx_peak_uptime_ms,
                (unsigned long long)s_output.mqtt.rx_peak_observed_until_uptime_ms,
                (unsigned)s_output.mqtt.complete_owner_count, (unsigned)s_output.mqtt.complete_payload_bytes,
                (unsigned)s_output.mqtt.partial_declared_bytes, (unsigned)s_output.mqtt.partial_received_bytes,
                (unsigned)s_output.mqtt.owned_message_metadata_bytes, (unsigned)s_output.mqtt.owned_request_bytes,
                (unsigned)s_output.mqtt.outbox_wire_bytes_at_rx_peak, (unsigned)s_output.mqtt.outbox_full_count,
                (unsigned)s_output.mqtt.last_outbox_full_payload_bytes, (unsigned long long)s_output.mqtt.last_outbox_full_uptime_ms,
                (unsigned)s_output.mqtt.notice_count_high_water, (unsigned)s_output.mqtt.notice_full_count,
                (unsigned)s_output.mqtt.counters_valid, (unsigned)s_output.mqtt.observation_storage_bytes, (unsigned)sizeof s_output.mqtt);
            s_output.stage = OUTPUT_COLLECT_REGIONS;
            return;
        case OUTPUT_COLLECT_REGIONS:
            heap_caps_walk_capacity(collect_region, NULL);
            s_output.index = 0;
            s_output.stage = OUTPUT_REGION;
            continue;
        case OUTPUT_REGION:
            if (s_output.index < s_output.stored_regions) {
                (void)emit_region(&s_work.regions[s_output.index++], &s_output.frame);
                return;
            }
            s_output.stage = OUTPUT_COLLECT_TASKS;
            continue;
        case OUTPUT_COLLECT_TASKS:
            collect_tasks();
            s_output.index = 0;
            s_output.stage = OUTPUT_TASK;
            continue;
        case OUTPUT_TASK:
            if (s_output.index < s_output.task_count && s_output.index < CAPACITY_TASK_LIMIT) {
                const size_t i = s_output.index++;
                const size_t stack = (size_t)s_work.tasks[i].usStackHighWaterMark * sizeof(StackType_t);
                if (stack < s_output.minimum_stack) s_output.minimum_stack = stack;
                char name_hex[configMAX_TASK_NAME_LEN * 2 + 1];
                static const char hex[] = "0123456789abcdef";
                for (unsigned j=0; j<configMAX_TASK_NAME_LEN; ++j) {
                    const unsigned char ch = (unsigned char)s_task_names[i][j];
                    name_hex[j*2] = hex[ch>>4]; name_hex[j*2+1] = hex[ch&15];
                }
                name_hex[configMAX_TASK_NAME_LEN*2] = 0;
                printf("ESP_BASE_CAPACITY_TASK boot_id=%s frame=%u instance=%u name_hex=%s minimum_stack_bytes=%u state=%u\n",
                    s_output.frame.boot_id, (unsigned)s_output.frame.frame, (unsigned)s_work.tasks[i].xTaskNumber,
                    name_hex, (unsigned)stack, (unsigned)s_work.tasks[i].eCurrentState);
                return;
            }
            s_output.index = 0;
            s_output.stage = OUTPUT_DOMAIN;
            continue;
        case OUTPUT_DOMAIN:
            if (s_output.index < CAPACITY_DOMAIN_COUNT) {
                const unsigned i = (unsigned)s_output.index++;
                printf("ESP_BASE_CAPACITY_DOMAIN boot_id=%s frame=%u caps=%08x alignment_bytes=4 minimum_free_lower_bound_bytes=%u largest_request_lower_bound_bytes=%u history=region_minima_conservative_bound\n",
                    s_output.frame.boot_id, (unsigned)s_output.frame.frame, (unsigned)s_masks[i],
                    (unsigned)s_output.frame.minimum_free_sum[i], (unsigned)s_output.frame.largest_lower_bound[i]);
                return;
            }
            s_output.stage = OUTPUT_END;
            continue;
        case OUTPUT_END:
            printf("ESP_BASE_CAPACITY_END boot_id=%s frame=%u regions=%u allocated_tasks=%u created_instances=%u finalized_instances=%u completed_stack_min_bytes=%u worst_completed_instance=%u minimum_stack_bytes=%u task_snapshot_complete=%u counters_valid=%u facts_valid=%u workspace_bytes=%u observation_cost_added_back=0\n",
                s_output.frame.boot_id, (unsigned)s_output.frame.frame, (unsigned)s_output.frame.region_count, (unsigned)s_output.task_count,
                (unsigned)s_output.task_stats.created_instances, (unsigned)s_output.task_stats.finalized_instances,
                s_output.task_stats.finalized_instances ? (unsigned)((size_t)s_output.task_stats.completed_minimum * sizeof(StackType_t)) : 0U,
                (unsigned)s_output.task_stats.worst_completed_instance, (unsigned)(s_output.minimum_stack == SIZE_MAX ? 0 : s_output.minimum_stack),
                (unsigned)s_output.complete, (unsigned)s_output.task_stats.counters_valid, (unsigned)s_output.frame.valid,
                (unsigned)(sizeof s_work + sizeof s_task_names + sizeof s_output + sizeof s_next_ms + sizeof s_frame + sizeof s_frame_overflow));
            s_output.active = false;
            return;
        }
    }
}

static void emit(const char *boot_id, const char *phase, uint64_t now_ms)
{
    /* A reset must not abandon a periodic frame or interleave two frames. */
    while (s_output.active) emit_step();
    start_frame(boot_id, phase, now_ms);
    while (s_output.active) emit_step();
}

void esp_base_capacity_poll(const char *boot_id, uint64_t now_ms)
{
    if (!s_output.active) {
        if (now_ms < s_next_ms) return;
        s_next_ms = now_ms <= UINT64_MAX - CAPACITY_PERIOD_MS ? now_ms + CAPACITY_PERIOD_MS : UINT64_MAX;
        start_frame(boot_id, "periodic", now_ms);
    }
    emit_step();
}

void esp_base_capacity_before_reset(const char *boot_id, uint64_t now_ms)
{
    emit(boot_id, "before_reset", now_ms);
}
