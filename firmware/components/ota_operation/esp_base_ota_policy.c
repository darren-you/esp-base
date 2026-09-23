// SPDX-License-Identifier: Apache-2.0
#include "esp_base_ota_policy.h"

eota_policy_t esp_base_ota_policy(bool trusted_time)
{
    return (eota_policy_t){
        .project_name = "esp_base",
        .chip_id = 0x0005, /* ESP_CHIP_ID_ESP32C3 */
        .ota_0_address_bytes = 0x20000,
        .ota_1_address_bytes = 0x200000,
        .ota_size_bytes = 0x1e0000,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 1000,
        .idle_timeout_ms = 30000,
        .total_timeout_ms = 300000,
        .trusted_time = trusted_time,
    };
}
