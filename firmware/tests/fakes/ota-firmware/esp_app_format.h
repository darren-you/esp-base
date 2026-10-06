#pragma once

#include <stdint.h>

#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432u

typedef struct {
    uint8_t magic;
    uint16_t chip_id;
} esp_image_header_t;

typedef struct {
    uint32_t magic_word;
    char project_name[32];
} esp_app_desc_t;
