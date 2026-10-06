#pragma once

#include <stdbool.h>

#include "esp_app_format.h"
#include "esp_partition.h"

bool esp_ota_check_rollback_is_possible(void);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,
                                             esp_app_desc_t *description);
