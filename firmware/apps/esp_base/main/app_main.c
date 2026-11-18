// SPDX-License-Identifier: Apache-2.0
// ESP Base 固件：只运行基座，没有业务层。业务固件用同样的方式调用 base_app_run 并传入自己的回调。
#include "base_app.h"

void app_main(void)
{
    base_app_run(NULL);
}
