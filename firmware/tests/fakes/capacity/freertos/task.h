#pragma once
#include "FreeRTOS.h"
typedef struct { const char *pcTaskName; UBaseType_t xTaskNumber; UBaseType_t usStackHighWaterMark; unsigned eCurrentState; } TaskStatus_t;
typedef struct {
    UBaseType_t created_instances;
    UBaseType_t finalized_instances;
    UBaseType_t completed_minimum;
    UBaseType_t worst_completed_instance;
    char worst_completed_name[configMAX_TASK_NAME_LEN];
    BaseType_t counters_valid;
} TaskCapacityStats_t;
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
UBaseType_t uxTaskGetCapacitySnapshot(TaskStatus_t*,UBaseType_t,TaskCapacityStats_t*);
