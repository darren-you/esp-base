#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
