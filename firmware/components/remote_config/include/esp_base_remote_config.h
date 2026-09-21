#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t generation;
} esp_base_remote_config_t;

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config);
