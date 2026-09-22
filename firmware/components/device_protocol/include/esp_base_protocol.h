#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_base_config.h"

typedef struct {
    const char *device_id;
    const char *firmware_version;
    const char *chip_model;
    uint32_t flash_size_bytes;
    esp_base_remote_config_t config;
    const char *reset_reason;
} esp_base_protocol_context_t;

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context);
