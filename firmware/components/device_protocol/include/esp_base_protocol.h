#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *device_id;
    const char *firmware_version;
    const char *chip_model;
    uint32_t flash_size_bytes;
    uint32_t config_generation;
    const char *reset_reason;
    bool provisioned;
} esp_base_protocol_context_t;

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context);
