// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_product_ledger.h"
#include "esp_base_storage_owner.h"

/* Each NVS operation holds the shared short Flash I/O owner only for its
 * physical read or commit. The caller separately holds the long product
 * transaction claim across the whole lifecycle. */
ebase_product_ledger_io_t ebase_product_ledger_nvs_io(
    esp_base_storage_owner_t *flash_io_owner);
