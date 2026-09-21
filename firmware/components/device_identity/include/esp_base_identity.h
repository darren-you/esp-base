#pragma once

#include <stdint.h>

#include "esp_err.h"

#define ESP_BASE_DEVICE_ID_LENGTH 37

typedef struct {
    char device_id[ESP_BASE_DEVICE_ID_LENGTH];
    const char *model;
    int revision;
    uint32_t flash_size_bytes;
    uint8_t base_mac[6];
} esp_base_identity_t;

esp_err_t esp_base_identity_read(esp_base_identity_t *identity);
