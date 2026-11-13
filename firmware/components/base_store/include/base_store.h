// SPDX-License-Identifier: Apache-2.0
// 持久数据：设备 UUID、Wi-Fi／MQTT 配置和 OTA 记录，都在默认 NVS 分区的 base 命名空间。
// 出厂数据由主机工具生成 NVS 镜像随固件刷入；任何错误都不擦除分区。
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "base_ota.h"
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_uri[128];
    char mqtt_username[65];
    char mqtt_password[129];
    uint32_t revision;
} base_config_t;

esp_err_t base_store_init(void);
/* 读取设备 UUID；没有时生成一个并保存。 */
esp_err_t base_store_device_id(char out[BASE_UUID_BYTES]);
/* 读取配置；没有刷入配置时返回 ESP_ERR_NVS_NOT_FOUND。 */
esp_err_t base_store_load_config(base_config_t *config);
/* 保存已验证可用的配置，revision 加一。 */
esp_err_t base_store_save_config(base_config_t *config);
esp_err_t base_store_load_ota(base_ota_record_t *record);
esp_err_t base_store_save_ota(const base_ota_record_t *record);
