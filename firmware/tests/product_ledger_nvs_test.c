// SPDX-License-Identifier: Apache-2.0
#include "esp_base_product_ledger_nvs.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <assert.h>
#include <string.h>

static esp_base_storage_owner_t owner;
static uint8_t persisted[EBASE_PRODUCT_LEDGER_BYTES];
static uint8_t staged[EBASE_PRODUCT_LEDGER_BYTES];
static bool exists, commit_fails, short_blob;
static unsigned active_handles, commits;

static void lease_held(void)
{
    assert(atomic_load(&owner.active_token) != 0U);
}

esp_err_t nvs_flash_init_partition(const char *partition)
{
    lease_held();
    assert(strcmp(partition, "base_store") == 0);
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *space,
                                  nvs_open_mode_t mode, nvs_handle_t *handle)
{
    lease_held();
    assert(strcmp(partition, "base_store") == 0);
    assert(strcmp(space, "base_product") == 0);
    if (mode == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    assert(active_handles == 0);
    *handle = mode == NVS_READONLY ? 1U : 2U;
    ++active_handles;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output, size_t *size)
{
    lease_held();
    assert(handle == 1U && active_handles == 1U);
    assert(strcmp(key, "operations") == 0);
    assert(*size == EBASE_PRODUCT_LEDGER_BYTES);
    memcpy(output, persisted, EBASE_PRODUCT_LEDGER_BYTES);
    *size = short_blob ? EBASE_PRODUCT_LEDGER_BYTES - 1U : EBASE_PRODUCT_LEDGER_BYTES;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *bytes, size_t size)
{
    lease_held();
    assert(handle == 2U && active_handles == 1U);
    assert(strcmp(key, "operations") == 0);
    assert(size == EBASE_PRODUCT_LEDGER_BYTES);
    memcpy(staged, bytes, size);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    lease_held();
    assert(handle == 2U && active_handles == 1U);
    ++commits;
    if (commit_fails) return ESP_FAIL;
    memcpy(persisted, staged, sizeof persisted);
    exists = true;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    lease_held();
    assert((handle == 1U || handle == 2U) && active_handles == 1U);
    --active_handles;
}

int main(void)
{
    esp_base_storage_owner_init(&owner);
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES] = {0};
    assert(io.read(io.context, bytes) == EBASE_LEDGER_IO_NOT_FOUND);
    assert(atomic_load(&owner.active_token) == 0U && active_handles == 0U);
    bytes[0] = 0x55;
    assert(io.write(io.context, bytes) == EBASE_LEDGER_IO_OK);
    assert(commits == 1U && atomic_load(&owner.active_token) == 0U);
    memset(bytes, 0, sizeof bytes);
    assert(io.read(io.context, bytes) == EBASE_LEDGER_IO_OK);
    assert(bytes[0] == 0x55 && active_handles == 0U);
    short_blob = true;
    assert(io.read(io.context, bytes) == EBASE_LEDGER_IO_ERROR);
    short_blob = false;
    commit_fails = true;
    assert(io.write(io.context, bytes) == EBASE_LEDGER_IO_ERROR);
    commit_fails = false;
    esp_base_storage_claim_t foreign = {0};
    assert(esp_base_storage_claim(&owner, &foreign));
    assert(io.read(io.context, bytes) == EBASE_LEDGER_IO_BUSY);
    assert(io.write(io.context, bytes) == EBASE_LEDGER_IO_BUSY);
    assert(esp_base_storage_release(&foreign));
    assert(active_handles == 0U && atomic_load(&owner.active_token) == 0U);
    return 0;
}
