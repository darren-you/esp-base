// SPDX-License-Identifier: Apache-2.0
#include "esp_base_product_ledger_nvs.h"

#include "nvs.h"
#include "nvs_flash.h"

#define PRODUCT_PARTITION "base_store"
#define PRODUCT_NAMESPACE "base_product"
#define PRODUCT_KEY "operations"

static ebase_ledger_io_result_t read_blob(
    void *context, uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    if (context == NULL) return EBASE_LEDGER_IO_ERROR;
    esp_base_storage_claim_t claim = {0};
    if (!esp_base_storage_claim(context, &claim)) return EBASE_LEDGER_IO_BUSY;
    esp_err_t error = nvs_flash_init_partition(PRODUCT_PARTITION);
    nvs_handle_t handle;
    if (error == ESP_OK) {
        error = nvs_open_from_partition(PRODUCT_PARTITION, PRODUCT_NAMESPACE,
                                        NVS_READONLY, &handle);
        if (error == ESP_OK) {
            size_t length = EBASE_PRODUCT_LEDGER_BYTES;
            error = nvs_get_blob(handle, PRODUCT_KEY, bytes, &length);
            if (error == ESP_OK && length != EBASE_PRODUCT_LEDGER_BYTES)
                error = ESP_FAIL;
            nvs_close(handle);
        }
    }
    const bool released = esp_base_storage_release(&claim);
    if (!released) return EBASE_LEDGER_IO_ERROR;
    if (error == ESP_ERR_NVS_NOT_FOUND) return EBASE_LEDGER_IO_NOT_FOUND;
    return error == ESP_OK ? EBASE_LEDGER_IO_OK : EBASE_LEDGER_IO_ERROR;
}

static ebase_ledger_io_result_t write_blob(
    void *context, const uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    if (context == NULL) return EBASE_LEDGER_IO_ERROR;
    esp_base_storage_claim_t claim = {0};
    if (!esp_base_storage_claim(context, &claim)) return EBASE_LEDGER_IO_BUSY;
    esp_err_t error = nvs_flash_init_partition(PRODUCT_PARTITION);
    nvs_handle_t handle;
    if (error == ESP_OK) {
        error = nvs_open_from_partition(PRODUCT_PARTITION, PRODUCT_NAMESPACE,
                                        NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            error = nvs_set_blob(handle, PRODUCT_KEY, bytes, EBASE_PRODUCT_LEDGER_BYTES);
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    const bool released = esp_base_storage_release(&claim);
    return released && error == ESP_OK ? EBASE_LEDGER_IO_OK : EBASE_LEDGER_IO_ERROR;
}

ebase_product_ledger_io_t ebase_product_ledger_nvs_io(
    esp_base_storage_owner_t *flash_io_owner)
{
    return (ebase_product_ledger_io_t){
        .read = read_blob, .write = write_blob, .context = flash_io_owner,
    };
}
