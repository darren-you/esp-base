#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_PARTITION_TYPE_APP 0u
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10u
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11u

typedef struct {
    uint8_t type;
    uint8_t subtype;
    uint32_t address;
    uint32_t size;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(uint8_t type, uint8_t subtype,
                                                 const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *destination, size_t size);
