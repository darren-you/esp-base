#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portTICK_PERIOD_MS 10
void test_mqtt_critical_enter(portMUX_TYPE *lock);
void test_mqtt_critical_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) test_mqtt_critical_enter(lock)
#define portEXIT_CRITICAL(lock) test_mqtt_critical_exit(lock)
