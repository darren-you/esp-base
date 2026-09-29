// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "eota.h"

#define ESP_BASE_OTA_OPERATION_ID_BYTES 37
#define ESP_BASE_OTA_PACKAGE_URL_BYTES 1024U
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define ESP_BASE_OTA_TARGET "esp32c3/esp_base"
#define ESP_BASE_OTA_SIGNATURE_SCHEME "esp_secure_boot_v2_rsa3072"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define ESP_BASE_OTA_TARGET "esp32/esp_base"
#define ESP_BASE_OTA_SIGNATURE_SCHEME "esp_secure_boot_v1_ecdsa_p256"
#define ESP_BASE_OTA_1_ADDRESS_BYTES 0x140000
#define ESP_BASE_OTA_SLOT_SIZE_BYTES 0x120000
#else
#error "ESP Base OTA policy supports only esp32c3 and esp32"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define ESP_BASE_OTA_1_ADDRESS_BYTES 0x150000
#define ESP_BASE_OTA_SLOT_SIZE_BYTES 0x130000
#endif
#define ESP_BASE_OTA_0_ADDRESS_BYTES 0x20000

typedef enum {
    ESP_BASE_OTA_NO_PACKAGE = 0,
    ESP_BASE_OTA_PACKAGE_REUSE = 1,
    ESP_BASE_OTA_PACKAGE_WRITE = 2,
} esp_base_ota_package_mode_t;

typedef struct {
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    char image_url[EOTA_URL_BYTES + 1];
    uint8_t sha256[EOTA_SHA256_BYTES];
    uint32_t image_size_bytes;
    esp_base_ota_package_mode_t package_mode;
    uint8_t package_sha256[EOTA_SHA256_BYTES];
    uint8_t trial_event_sha256[EOTA_SHA256_BYTES];
    uint32_t package_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
    char package_url[ESP_BASE_OTA_PACKAGE_URL_BYTES + 1U];
} esp_base_ota_request_t;

/* Fixed, trusted Base product/partition policy; never derive it from ota.start. */
/* Bind once at boot, before any OTA action. Unbound writes fail closed. */
bool esp_base_ota_policy_bind_flash_io(eota_flash_io_t flash_io);
eota_policy_t esp_base_ota_policy(bool trusted_time);
