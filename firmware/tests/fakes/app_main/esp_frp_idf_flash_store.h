#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_frp_flash_reader.h"

#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_UNDEFINED 6

typedef struct {
    const char *partition_label;
    int partition_type;
    int partition_subtype;
    uint32_t partition_offset_bytes;
    uint32_t partition_size_bytes;
    void *owner_context;
    efrp_result_t (*with_owner)(void *owner_context,
                                efrp_result_t (*operation)(void *),
                                void *operation_context);
} efrp_idf_flash_store_config_t;

typedef struct {
    efrp_idf_flash_store_config_t config;
} efrp_idf_flash_store_t;

bool efrp_idf_flash_store_bind(efrp_idf_flash_store_t *store,
                                const efrp_idf_flash_store_config_t *config);
const efrp_aead_flash_store_t *efrp_idf_flash_store_callbacks(
    const efrp_idf_flash_store_t *store);
