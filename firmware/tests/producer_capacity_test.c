#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/task.h"

static char output[131072];
static size_t output_length;
static unsigned suspend_depth, walks, snapshots;
static bool begin_seen;
static heap_capacity_region_stats_t regions[8];
static size_t region_count;
static UBaseType_t live_count;
static TaskCapacityStats_t task_facts;
static char *live_names[32];
static UBaseType_t live_hwm[32];

int esp_rom_printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int count = vsnprintf(output + output_length, sizeof(output) - output_length, format, ap);
    va_end(ap);
    assert(count >= 0 && (size_t)count < sizeof(output) - output_length);
    output_length += (size_t)count;
    if (strstr(format, "ESP_BASE_CAPACITY_BEGIN ") == format) begin_seen = true;
    return count;
}
void heap_caps_walk_capacity(heap_capacity_region_cb_t callback, void *user)
{
    assert(begin_seen);
    ++walks;
    for (size_t i = 0; i < region_count; ++i) assert(callback(&regions[i], user));
}
void vTaskSuspendAll(void) { ++suspend_depth; }
BaseType_t xTaskResumeAll(void)
{
    assert(suspend_depth > 0);
    if (--suspend_depth == 0) {
        /* Real stopped TCB names may disappear as soon as the scheduler resumes. */
        for (unsigned i = 0; i < 32; ++i) {
            free(live_names[i]);
            live_names[i] = NULL;
        }
    }
    return 0;
}
UBaseType_t uxTaskGetCapacitySnapshot(TaskStatus_t *tasks, UBaseType_t limit, TaskCapacityStats_t *stats)
{
    assert(begin_seen && suspend_depth == 1);
    ++snapshots;
    vTaskSuspendAll();
    assert(live_count <= limit);
    for (UBaseType_t i = 0; i < live_count; ++i) {
        tasks[i].pcTaskName = live_names[i];
        tasks[i].xTaskNumber = i + 40;
        tasks[i].usStackHighWaterMark = live_hwm[i];
        tasks[i].eCurrentState = i;
    }
    *stats = task_facts;
    xTaskResumeAll();
    return live_count;
}
#include "../components/device_protocol/esp_base_capacity.c"

static void reset_case(void)
{
    assert(suspend_depth == 0);
    memset(regions, 0, sizeof regions);
    memset(output, 0, sizeof output);
    output_length = 0;
    begin_seen = false;
    walks = snapshots = 0;
    region_count = live_count = 0;
    task_facts = (TaskCapacityStats_t){.completed_minimum=UINT32_MAX, .counters_valid=1};
    memset(live_hwm, 0, sizeof live_hwm);
    for (unsigned i = 0; i < 32; ++i) assert(live_names[i] == NULL);
}
static void alive(UBaseType_t count)
{
    live_count = count;
    for (UBaseType_t i = 0; i < count; ++i) {
        live_names[i] = calloc(1, configMAX_TASK_NAME_LEN);
        assert(live_names[i]);
        memcpy(live_names[i], "worker", 6);
        live_hwm[i] = 1400 + i * 400;
    }
}
static void add_region(uint32_t first, uint32_t second, uint32_t third, bool early, size_t free_min, size_t largest_min)
{
    heap_capacity_region_stats_t *region = &regions[region_count];
    region->start = 0x10000 + (intptr_t)region_count * 0x20000;
    region->end = region->start + 0x10000;
    region->caps[0] = first; region->caps[1] = second; region->caps[2] = third;
    region->available_at_heap_init = early;
    region->allocator = (multi_heap_capacity_stats_t){.current_free_bytes=free_min+500,
        .minimum_free_bytes=free_min, .current_largest_allocatable_bytes=largest_min+100,
        .minimum_largest_allocatable_bytes=largest_min, .allocator_metadata_bytes=100};
    if (region_count == 1) {
        region->alias_start = 0x700000;
        region->alias_end = 0x710000;
        region->alias_inverted = true;
    }
    ++region_count;
}
static const char *line(const char *marker)
{
    const char *p = strstr(output, marker);
    assert(p);
    return p;
}
static unsigned long long value(const char *p, const char *key)
{
    char token[96];
    snprintf(token, sizeof token, " %s=", key);
    const char *at = strstr(p, token);
    assert(at && at < strchr(p, '\n'));
    return strtoull(at + strlen(token), NULL, 10);
}
static void domain(uint32_t caps, size_t free_min, size_t largest_min)
{
    char token[64];
    snprintf(token, sizeof token, "caps=%08x alignment_bytes=4 ", caps);
    const char *p = line(token);
    assert(value(p, "minimum_free_lower_bound_bytes") == free_min);
    assert(value(p, "largest_request_lower_bound_bytes") == largest_min);
}
static void run(void)
{
    emit("software-test-boot", "periodic", 1234);
    assert(walks == 1 && snapshots == 1 && suspend_depth == 0);
    const char *begin = line("ESP_BASE_CAPACITY_BEGIN ");
    const char *end = line("ESP_BASE_CAPACITY_END ");
    assert(begin == output && begin < end);
    assert(value(begin,"uptime_ms") == 1234);
    assert(value(end,"regions") == region_count);
    assert(value(end,"allocated_tasks") == live_count);
    assert(value(end,"observation_cost_added_back") == 0);
}
int main(void)
{
    reset_case();
    uint32_t byte_caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DEFAULT|MALLOC_CAP_DMA|MALLOC_CAP_32BIT;
    add_region(MALLOC_CAP_8BIT|MALLOC_CAP_DEFAULT, MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_32BIT, 0, true, 22000, 26000);
    add_region(MALLOC_CAP_8BIT|MALLOC_CAP_DEFAULT, MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA, MALLOC_CAP_32BIT, true, 6000, 12000);
    add_region(byte_caps, 0, 0, false, 999999, 900000);
    add_region(MALLOC_CAP_INTERNAL|MALLOC_CAP_IRAM_8BIT|MALLOC_CAP_EXEC|MALLOC_CAP_32BIT, 0, 0, true, 9000, 8800);
    alive(2);
    task_facts.created_instances=5; task_facts.finalized_instances=3; task_facts.completed_minimum=1200;
    run();
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,28000,26000);
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_32BIT,37000,26000);
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA,28000,26000);
    domain(MALLOC_CAP_DEFAULT,28000,26000);
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_IRAM_8BIT,9000,8800);
    assert(value(line("ESP_BASE_CAPACITY_END "),"task_snapshot_complete") == 1);
    assert(value(line("ESP_BASE_CAPACITY_END "),"minimum_stack_bytes") == 1200);
    assert(strstr(output,"name_hex=776f726b657200000000000000000000"));
    puts("PASS real-emitter: late-region zero for both bounds; caps union once; aliases not duplicated; IRAM distinct; names survive resume");

    reset_case();
    add_region(byte_caps,0,0,false,999999,900000);
    alive(2); task_facts.created_instances=3;
    run();
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,0,0);
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_32BIT,0,0);
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA,0,0);
    domain(MALLOC_CAP_DEFAULT,0,0);
    domain(MALLOC_CAP_INTERNAL|MALLOC_CAP_IRAM_8BIT,0,0);
    assert(value(line("ESP_BASE_CAPACITY_END "),"task_snapshot_complete") == 0);
    assert(value(line("ESP_BASE_CAPACITY_END "),"counters_valid") == 1);
    assert(value(line("ESP_BASE_CAPACITY_END "),"completed_stack_min_bytes") == 0);
    puts("PASS real-emitter: absent prefix never raises free/largest; pending cleanup remains incomplete without permanent invalid flag");

    reset_case(); run();
    assert(value(line("ESP_BASE_CAPACITY_END "),"task_snapshot_complete") == 0);
    assert(value(line("ESP_BASE_CAPACITY_END "),"minimum_stack_bytes") == 0);
    puts("PASS real-emitter: zero/insufficient snapshot and empty-final sentinel cannot qualify");

    reset_case(); alive(1); task_facts.created_instances=1; task_facts.counters_valid=0; run();
    assert(value(line("ESP_BASE_CAPACITY_END "),"task_snapshot_complete") == 0);
    puts("PASS real-emitter: persistent counter invalid blocks complete snapshot");

    reset_case(); alive(1); task_facts.created_instances=1; live_hwm[0]=896; run();
    assert(value(line("ESP_BASE_CAPACITY_END "),"task_snapshot_complete") == 1);
    assert(value(line("ESP_BASE_CAPACITY_END "),"minimum_stack_bytes") == 896);
    puts("PASS real-emitter: complete coverage retains below-1024 stack result (decoder must reject threshold)");

    reset_case(); alive(1); task_facts.created_instances=1; task_facts.finalized_instances=2; task_facts.completed_minimum=1200; run();
    assert(value(line("ESP_BASE_CAPACITY_END "),"task_snapshot_complete") == 0);
    puts("PASS real-emitter: finalized greater than created rejects without unsigned underflow");

    s_frame=UINT32_MAX-1; s_frame_overflow=false;
    reset_case(); run(); assert(value(line("ESP_BASE_CAPACITY_END "),"facts_valid") == 1);
    reset_case(); run(); assert(value(line("ESP_BASE_CAPACITY_END "),"facts_valid") == 0);
    reset_case(); run(); assert(value(line("ESP_BASE_CAPACITY_END "),"facts_valid") == 0);
    puts("PASS real-emitter: frame saturation permanently invalidates facts; BEGIN precedes every getter");
    puts("Software emitter tests only; host fake storage sizes and timing grant no MCU/R5/R6 qualification.");
    return 0;
}
