// SPDX-License-Identifier: Apache-2.0
// 公网出口地址：经 HTTPS 访问回显地址（CONFIG_ESP_BASE_PUBLIC_IP_URL），应答体就是对端看到的来源地址。
// 每次在一次性的任务里取一次，取完回调后任务退出，不常驻栈。
#pragma once
#include <stdbool.h>

/* 取到时 ip 是 malloc 的地址文本，没取到时为 NULL；在取地址的任务里调用，只应转发事件，ip 由接收方释放。 */
typedef void (*base_public_ip_cb_t)(char *ip);

/* 开始取一次。回显地址没有配置、上一次还没结束或者任务建不起来时返回 false，不会回调。 */
bool base_public_ip_start(base_public_ip_cb_t done);
