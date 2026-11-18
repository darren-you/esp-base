// SPDX-License-Identifier: Apache-2.0
// ESP Base 固件：只运行系统层，没有业务层。业务固件用同样的方式调用 base_system_run 并交出自己的业务描述。
#include "base_system.h"

void app_main(void)
{
    base_system_run(NULL);
}
