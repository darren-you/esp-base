// SPDX-License-Identifier: Apache-2.0
#include "esp_base_ota_policy.h"

eota_policy_t esp_base_ota_policy(bool trusted_time)
{
    return (eota_policy_t){
        .project_name = "esp_base",
        .chip_id = CONFIG_IDF_FIRMWARE_CHIP_ID,
        .ota_0_address_bytes = ESP_BASE_OTA_0_ADDRESS_BYTES,
        .ota_1_address_bytes = ESP_BASE_OTA_1_ADDRESS_BYTES,
        .ota_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 1000,
        .idle_timeout_ms = 30000,
        .total_timeout_ms = 300000,
        .trusted_time = trusted_time,
    };
}
