#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
#define pdPASS 1
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack_depth,
                       void *argument, UBaseType_t priority, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
