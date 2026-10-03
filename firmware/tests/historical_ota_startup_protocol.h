// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_product_ledger.h"
#include "esp_base_protocol.h"

/* Test setup only: the startup shim supplies app_main's actual owner pointers.
 * Recovery, ledger preparation and boot_id keep their production functions. */
void historical_ota_protocol_seed(const esp_base_protocol_context_t *context,
                                  const char boot_id[EBASE_PRODUCT_ID_BYTES]);

void historical_ota_ledger_reset(void);
ebase_product_ledger_result_t historical_ota_ledger_initialize(
    esp_base_storage_owner_t *flash_io_owner);
ebase_product_ledger_result_t historical_ota_ledger_begin(
    esp_base_storage_owner_t *flash_io_owner,
    const ebase_product_record_t *intent);
ebase_product_ledger_result_t historical_ota_ledger_read(
    esp_base_storage_owner_t *flash_io_owner, ebase_product_ledger_t *ledger);
unsigned historical_ota_ledger_write_count(void);
