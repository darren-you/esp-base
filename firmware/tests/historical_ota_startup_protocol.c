// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "fakes/app_main/esp_partition.h"
#include "fakes/protocol-path/freertos/task.h"

/* app_main's platform shims retain the original names. Keep the production
 * recovery, prepare_product_ledger and boot_id functions unchanged. Compile
 * with function/data sections and linker GC: no USB/MQTT worker is executed. */
#define esp_base_protocol_load_config historical_ota_unused_protocol_load_config
#define esp_base_protocol_start historical_ota_unused_protocol_start
#define esp_base_protocol_control_healthy historical_ota_unused_protocol_control_healthy
#define esp_base_protocol_control_progress_count historical_ota_unused_protocol_control_progress_count
#define esp_base_protocol_begin_firmware_package_verification historical_ota_unused_protocol_begin_firmware_package_verification
#define esp_base_protocol_firmware_package_health_snapshot historical_ota_unused_protocol_firmware_package_health_snapshot
#define esp_base_protocol_end_firmware_package_verification historical_ota_unused_protocol_end_firmware_package_verification
#define esp_base_protocol_set_ota_verification_pending historical_ota_unused_protocol_set_ota_verification_pending
#include "../components/device_protocol/esp_base_protocol.c"
#undef esp_base_protocol_load_config
#undef esp_base_protocol_start
#undef esp_base_protocol_control_healthy
#undef esp_base_protocol_control_progress_count
#undef esp_base_protocol_begin_firmware_package_verification
#undef esp_base_protocol_firmware_package_health_snapshot
#undef esp_base_protocol_end_firmware_package_verification
#undef esp_base_protocol_set_ota_verification_pending

#include "historical_ota_startup_protocol.h"

bool historical_product_health_window(uint64_t now,
    const uint8_t package_sha256[32], uint64_t *event_sequence,
    uint64_t *failure_count, uint64_t *stable_since_ms, uint64_t *last_poll_ms)
{
    return observe_business_trial_window(now, package_sha256, event_sequence,
        failure_count, stable_since_ms, last_poll_ms);
}

static uint8_t historical_ledger_bytes[EBASE_PRODUCT_LEDGER_BYTES];
static bool historical_ledger_present;
static unsigned historical_ledger_writes;

/* Same in-memory NVS substitute as protocol_ota_owner_test. The actual ledger
 * still encodes, validates its CRC and independently reads every commit back. */
static ebase_ledger_io_result_t historical_ledger_read(
    void *context, uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    assert(context != NULL && bytes != NULL);
    if (!historical_ledger_present) return EBASE_LEDGER_IO_NOT_FOUND;
    memcpy(bytes, historical_ledger_bytes, sizeof historical_ledger_bytes);
    return EBASE_LEDGER_IO_OK;
}

static ebase_ledger_io_result_t historical_ledger_write(
    void *context, const uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    assert(context != NULL && bytes != NULL);
    memcpy(historical_ledger_bytes, bytes, sizeof historical_ledger_bytes);
    historical_ledger_present = true;
    ++historical_ledger_writes;
    return EBASE_LEDGER_IO_OK;
}

ebase_product_ledger_io_t ebase_product_ledger_nvs_io(
    esp_base_storage_owner_t *flash_io_owner)
{
    assert(flash_io_owner != NULL);
    return (ebase_product_ledger_io_t){historical_ledger_read,
        historical_ledger_write, flash_io_owner};
}

#if defined(CONFIG_IDF_TARGET_ESP32)
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT));
    return malloc(size);
}
#endif

void historical_ota_protocol_seed(const esp_base_protocol_context_t *context,
                                  const char boot_id[EBASE_PRODUCT_ID_BYTES])
{
    assert(context != NULL && context->storage_owner != NULL &&
           context->flash_io_owner != NULL &&
           ebase_is_uuid(context->device_id) && ebase_is_uuid(boot_id));
    s_context = (protocol_state_t){
        .device_id = context->device_id,
        .firmware_version = context->firmware_version,
        .chip_model = context->chip_model,
        .flash_size_bytes = context->flash_size_bytes,
        .reset_reason = context->reset_reason,
        .storage_owner = context->storage_owner,
        .flash_io_owner = context->flash_io_owner,
        .frp_flash_store = context->frp_flash_store,
    };
    memcpy(s_boot_id, boot_id, sizeof s_boot_id);
    s_config_loaded = true;
    s_started = true;
}

void historical_ota_ledger_reset(void)
{
    memset(historical_ledger_bytes, 0, sizeof historical_ledger_bytes);
    historical_ledger_present = false;
    historical_ledger_writes = 0U;
}

ebase_product_ledger_result_t historical_ota_ledger_initialize(
    esp_base_storage_owner_t *flash_io_owner)
{
    ebase_product_ledger_t ledger = {0};
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(flash_io_owner);
    return ebase_product_ledger_initialize_empty(&ledger, &io);
}

ebase_product_ledger_result_t historical_ota_ledger_begin(
    esp_base_storage_owner_t *flash_io_owner,
    const ebase_product_record_t *intent)
{
    ebase_product_ledger_t ledger;
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(&ledger, &io);
    return opened == EBASE_LEDGER_OK ? ebase_product_ledger_begin(&ledger, &io, intent) :
                                      opened;
}

ebase_product_ledger_result_t historical_ota_ledger_read(
    esp_base_storage_owner_t *flash_io_owner, ebase_product_ledger_t *ledger)
{
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(flash_io_owner);
    return ebase_product_ledger_open(ledger, &io);
}

unsigned historical_ota_ledger_write_count(void)
{
    return historical_ledger_writes;
}
