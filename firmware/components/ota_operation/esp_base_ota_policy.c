// SPDX-License-Identifier: Apache-2.0
#include "esp_base_ota_policy.h"

#include <stddef.h>

static bool unbound_flash_io(void *context)
{
    (void)context;
    return false;
}

static eota_flash_io_t s_flash_io = {
    .acquire = unbound_flash_io,
    .release = unbound_flash_io,
};
static bool s_flash_io_bound;

bool esp_base_ota_policy_bind_flash_io(eota_flash_io_t flash_io)
{
    if (s_flash_io_bound || flash_io.acquire == NULL || flash_io.release == NULL ||
        flash_io.context == NULL) return false;
    s_flash_io = flash_io;
    s_flash_io_bound = true;
    return true;
}

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
        .flash_io = s_flash_io,
    };
}
