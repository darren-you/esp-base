#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_EXEC (1U<<0)
#define MALLOC_CAP_32BIT (1U<<1)
#define MALLOC_CAP_8BIT (1U<<2)
#define MALLOC_CAP_DMA (1U<<3)
#define MALLOC_CAP_INTERNAL (1U<<11)
#define MALLOC_CAP_DEFAULT (1U<<12)
#define MALLOC_CAP_IRAM_8BIT (1U<<13)
#define HEAP_CAPACITY_PRIORITIES 3
typedef struct {
    size_t current_free_bytes;
    size_t minimum_free_bytes;
    size_t current_largest_allocatable_bytes;
    size_t minimum_largest_allocatable_bytes;
    size_t allocator_metadata_bytes;
} multi_heap_capacity_stats_t;
typedef struct {
    intptr_t start;
    intptr_t end;
    uint32_t caps[HEAP_CAPACITY_PRIORITIES];
    uintptr_t alias_start;
    uintptr_t alias_end;
    bool alias_inverted;
    bool available_at_heap_init;
    multi_heap_capacity_stats_t allocator;
} heap_capacity_region_stats_t;
typedef bool (*heap_capacity_region_cb_t)(const heap_capacity_region_stats_t*,void*);
void heap_caps_walk_capacity(heap_capacity_region_cb_t,void*);
