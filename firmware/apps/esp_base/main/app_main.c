// SPDX-License-Identifier: Apache-2.0
// ESP Base 固件：只运行系统层，没有业务层。业务固件用同样的方式调用 base_system_run 并交出自己的业务描述。
#include "base_system.h"
#include "sdkconfig.h"

#if CONFIG_ESP_BASE_TEST_BUSINESS_PANIC || CONFIG_ESP_BASE_TEST_BUSINESS_HANG
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 测试开关：一个启动 20 秒后出故障的业务，用来在实板上验证业务看门狗和安全模式。正式打包拒绝带这个开关的镜像。
static void test_task(void *argument)
{
    (void)argument;
    for (int i = 0; i < 4; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        base_runtime_feed();
    }
#if CONFIG_ESP_BASE_TEST_BUSINESS_PANIC
    abort();
#else
    for (;;) vTaskDelay(portMAX_DELAY); /* 不再报到：10 秒后运行时重启设备 */
#endif
}

static void test_init(void)
{
    base_runtime_task("test_fault", test_task, NULL, 3072, 3, 10000);
}

static const base_business_t s_test_business = {.name = "test_fault", .init = test_init};
#define BUSINESS (&s_test_business)
#else
#define BUSINESS NULL
#endif

void app_main(void)
{
    base_system_run(BUSINESS);
}
