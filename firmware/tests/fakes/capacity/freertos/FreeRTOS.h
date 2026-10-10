#pragma once
#include <stdint.h>
typedef uint32_t UBaseType_t;
typedef int32_t BaseType_t;
typedef uint8_t StackType_t;
#define configMAX_TASK_NAME_LEN 16
#define CONFIG_FREERTOS_SMP 0
#define CONFIG_HEAP_TLSF_USE_ROM_IMPL 0
#define CONFIG_HEAP_TASK_TRACKING 0
#define CONFIG_HEAP_POISONING_DISABLED 1
