// SPDX-License-Identifier: Apache-2.0
// 联网：Wi-Fi STA（默认省电模式）、IPv4／IPv6 和 SNTP。断线后按退避自动重连，全部由事件和定时器驱动。
// 地址、信号等设备信息由 base_profile 读取，这里不提供。
#pragma once
#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    BASE_NET_UP,   /* 取得 IP */
    BASE_NET_DOWN, /* Wi-Fi 断开 */
    BASE_NET_TIME, /* 时间已同步，可以校验 TLS 证书有效期 */
    BASE_NET_IP6,  /* 取得 IPv6 地址（链路本地或 SLAAC） */
} base_net_event_t;
/* 在系统事件任务或定时器任务里调用，只应转发事件。 */
typedef void (*base_net_cb_t)(base_net_event_t event);

esp_err_t base_net_start(const char *ssid, const char *password, base_net_cb_t callback);
/* 换用新的 Wi-Fi 凭据并重连。 */
esp_err_t base_net_reconfigure(const char *ssid, const char *password);
bool base_net_time_valid(void);
/* 开关 Wi-Fi 省电（modem sleep）。省电时下行包要等 DTIM 才送达，大文件下载期间应关闭。 */
void base_net_set_power_save(bool enabled);
