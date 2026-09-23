#pragma once
#include "esp_err.h"
esp_err_t nvs_flash_init_partition(const char *);
esp_err_t nvs_flash_init(void);
