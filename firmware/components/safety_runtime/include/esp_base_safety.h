#pragma once

#include "esp_err.h"

typedef struct {
    const char *reset_reason;
} esp_base_safety_t;

esp_err_t esp_base_safety_start(esp_base_safety_t *safety);
