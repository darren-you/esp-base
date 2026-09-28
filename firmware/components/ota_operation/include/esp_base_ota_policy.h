// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "eota.h"

#define ESP_BASE_OTA_OPERATION_ID_BYTES 37
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define ESP_BASE_OTA_TARGET "esp32c3/esp_base"
#define ESP_BASE_OTA_SIGNATURE_SCHEME "esp_secure_boot_v2_rsa3072"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#error "ESP32 Base OTA policy needs its frozen layout and signed boot chain"
#else
#error "ESP Base OTA policy supports only esp32c3 and esp32"
#endif

typedef struct {
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    char image_url[EOTA_URL_BYTES + 1];
    uint8_t sha256[EOTA_SHA256_BYTES];
    uint32_t image_size_bytes;
} esp_base_ota_request_t;

/* Fixed, trusted Base product/partition policy; never derive it from ota.start. */
eota_policy_t esp_base_ota_policy(bool trusted_time);
