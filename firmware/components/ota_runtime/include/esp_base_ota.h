#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    const char *running_partition;
    bool pending_verify;
} esp_base_ota_t;

esp_err_t esp_base_ota_inspect(esp_base_ota_t *ota);
esp_err_t esp_base_ota_mark_valid_if_pending(const esp_base_ota_t *ota);
