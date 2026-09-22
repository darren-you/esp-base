// SPDX-License-Identifier: Apache-2.0
#include "mqtt_lab_resources.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if !CONFIG_FREERTOS_USE_TRACE_FACILITY || !CONFIG_ESP_TIMER_PROFILING
#error "MQTT lab resource measurements require the app sdkconfig.defaults trace/profiling options"
#endif

static TaskStatus_t tasks[32];
static struct { char name[configMAX_TASK_NAME_LEN]; unsigned stack_bytes; } task_facts[32];

void ebase_mqtt_lab_report_resources(const char *phase, unsigned cycle)
{
    /* 描述符范围来自 IDF 的 lwIP 配置；SO_TYPE 只读，不开关任何 socket。
     * SDK 网络任务可能并发改变连接，因此这是有界扫描，不是原子全网快照。 */
    unsigned sockets = 0, socket_errors = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
        int type = 0;
        socklen_t length = sizeof(type);
        if (lwip_getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0) ++sockets;
        else if (errno != EBADF) ++socket_errors;
    }
    /* TaskStatus 的 name 是借用指针；C3 单核暂停调度期间复制，打印时不借用 TCB。 */
    vTaskSuspendAll();
    const UBaseType_t count = uxTaskGetSystemState(tasks, 32, NULL);
    for (UBaseType_t i = 0; i < count; ++i) {
        strncpy(task_facts[i].name, tasks[i].pcTaskName, sizeof(task_facts[i].name) - 1);
        task_facts[i].name[sizeof(task_facts[i].name) - 1] = '\0';
        task_facts[i].stack_bytes = (unsigned)tasks[i].usStackHighWaterMark * sizeof(StackType_t);
    }
    (void)xTaskResumeAll();
    printf("EBASE_MQTT_RESOURCE phase=%s cycle=%u heap=%" PRIu32 " min_heap=%" PRIu32
           " largest=%u tasks=%u sockets=%u socket_errors=%u\n", phase, cycle,
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)count, sockets, socket_errors);
    for (UBaseType_t i = 0; i < count; ++i)
        printf("EBASE_MQTT_TASK phase=%s cycle=%u name=%s stack_bytes=%u\n",
               phase, cycle, task_facts[i].name, task_facts[i].stack_bytes);
    printf("EBASE_MQTT_TIMERS_BEGIN phase=%s cycle=%u\n", phase, cycle);
    const esp_err_t error = esp_timer_dump(stdout);
    printf("EBASE_MQTT_TIMERS_END phase=%s cycle=%u error=%d\n", phase, cycle, error);
    fflush(stdout);
}
