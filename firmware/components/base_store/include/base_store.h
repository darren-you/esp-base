// SPDX-License-Identifier: Apache-2.0
// 持久数据：设备 UUID、Wi-Fi／MQTT 配置、名称与 Group 和 OTA 记录在默认 NVS 分区的 base 命名空间，业务参数在 business 命名空间。
// 出厂数据由主机工具生成 NVS 镜像随固件刷入；任何错误都不擦除分区。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_ota.h"
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    /* 系统 MQTT：esp-base 连本协议 Broker 的地址和账户，只由出厂资料写入。 */
    char mqtt_uri[128];
    char mqtt_username[65];
    char mqtt_password[129];
    /* 业务 MQTT：经 config 命令下发，留给业务层另开一条连接；esp-base 自己不连接。未配置时各字段为空。 */
    base_mqtt_t business_mqtt;
    /* 设备名称与所属 Group：经 config 命令下发，名称也可由出厂资料写入。为空表示没有设置（名称上报默认值）。 */
    char name[BASE_NAME_BYTES];
    char group[BASE_UUID_BYTES];
    uint32_t revision;
} base_config_t;

esp_err_t base_store_init(void);
/* 读取设备 UUID；没有时生成一个并保存。 */
esp_err_t base_store_device_id(char out[BASE_UUID_BYTES]);
/* 读取配置；没有刷入出厂配置时返回 ESP_ERR_NVS_NOT_FOUND。业务 MQTT 可以没有。 */
esp_err_t base_store_load_config(base_config_t *config);
/* 保存配置，revision 加一。 */
esp_err_t base_store_save_config(base_config_t *config);
/* 业务参数：出厂资料里 business 对象写进 NVS 的 business 命名空间，全是字符串，由业务层自己解释。
 * 没有这个键时返回 ESP_ERR_NVS_NOT_FOUND，out 为空串。 */
esp_err_t base_store_business_string(const char *key, char *out, size_t size);
esp_err_t base_store_load_ota(base_ota_record_t *record);
esp_err_t base_store_save_ota(const base_ota_record_t *record);
