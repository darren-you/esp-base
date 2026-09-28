#pragma once
#include <stddef.h>
#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_IRAM_8BIT 2
size_t heap_caps_get_minimum_free_size(unsigned caps);
void *heap_caps_malloc(size_t size, unsigned caps);
