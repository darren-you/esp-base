#include "esp_base_ota_receipt.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "psa/crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEVICE "22222222-2222-4222-8222-222222222222"
#define OP "44444444-4444-4444-8444-444444444444"
#define NEXT_OP "55555555-5555-4555-8555-555555555555"
#define IMAGE_BYTES 512
#define STORED_BYTES 118

static const esp_partition_t old_slot = {.type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .address = 0x20000, .size = 0x1e0000};
static const esp_partition_t new_slot = {.type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1, .address = 0x200000, .size = 0x1e0000};
static const esp_partition_t *running, *boot;
static esp_ota_img_states_t source_state, target_state;
static esp_err_t target_lookup;
static uint8_t image[IMAGE_BYTES], stored[STORED_BYTES], staged[STORED_BYTES];
static bool exists, signed_enabled, after_write;
static int fault, writes, commits, reads, partition_reads, handles;
enum { NO_FAULT, INIT_FAULT, READ_FAULT, OPEN_WRITE_FAULT, SET_BEFORE_FAULT, SET_AFTER_FAULT, COMMIT_FAULT, READBACK_FAULT, READBACK_MISMATCH };

static void reset(void)
{
    running = boot = &old_slot;
    source_state = ESP_OTA_IMG_VALID;
    target_state = ESP_OTA_IMG_UNDEFINED;
    target_lookup = ESP_ERR_NOT_FOUND;
    memset(image, 0x35, sizeof image);
    memset(stored, 0, sizeof stored);
    memset(staged, 0, sizeof staged);
    exists = false;
    signed_enabled = true;
    after_write = false;
    fault = writes = commits = reads = partition_reads = handles = 0;
}

static esp_base_ota_update_request_t request(const char *id)
{
    esp_base_ota_update_request_t out = {.image_size_bytes = IMAGE_BYTES};
    strcpy(out.operation_id, id);
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof image; ++i) sum += image[i];
    for (size_t i = 0; i < 32; ++i) out.sha256[i] = (uint8_t)(sum + i);
    return out;
}

bool esp_base_ota_update_available(void) { return signed_enabled; }
const char *esp_base_ota_update_error(esp_base_ota_update_result_t result)
{
    switch (result) {
    case ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED: return "ota_download_failed";
    case ESP_BASE_OTA_UPDATE_RESOURCE_FAILURE: return "resource_failure";
    default: return "other_failure";
    }
}
const esp_partition_t *esp_ota_get_running_partition(void) { return running; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return boot; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *partition)
{ assert(partition == NULL); return running == &old_slot ? &new_slot : &old_slot; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    assert(partition == &old_slot || partition == &new_slot);
    if (partition == running) { *state = source_state; return ESP_OK; }
    *state = target_state;
    return target_lookup;
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *output, size_t size)
{
    assert(partition == running && offset + size <= sizeof image);
    ++partition_reads;
    memcpy(output, image + offset, size);
    return ESP_OK;
}
esp_err_t nvs_flash_init_partition(const char *partition)
{ assert(!strcmp(partition, "base_store")); return fault == INIT_FAULT ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_open_from_partition(const char *partition, const char *space, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "base_store") && !strcmp(space, "base_ota"));
    if (mode == NVS_READWRITE && fault == OPEN_WRITE_FAULT) return ESP_FAIL;
    if (mode == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    ++handles;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 && handles > 0); --handles; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output, size_t *size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "operation"));
    ++reads;
    if (fault == READ_FAULT || (fault == READBACK_FAULT && after_write)) return ESP_FAIL;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < sizeof stored) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(output, stored, sizeof stored);
    if (fault == READBACK_MISMATCH && after_write) ((uint8_t *)output)[0] ^= 1;
    *size = sizeof stored;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "operation") && size == sizeof stored);
    ++writes;
    if (fault == SET_BEFORE_FAULT) return ESP_FAIL;
    memcpy(staged, data, size);
    after_write = true;
    if (fault == SET_AFTER_FAULT) { memcpy(stored, staged, size); exists = true; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && handles == 1);
    ++commits;
    if (fault == COMMIT_FAULT) return ESP_FAIL;
    memcpy(stored, staged, sizeof stored);
    exists = true;
    return ESP_OK;
}
psa_status_t psa_hash_setup(psa_hash_operation_t *operation, int algorithm)
{ assert(algorithm == PSA_ALG_SHA_256); operation->sum = 0; return PSA_SUCCESS; }
psa_status_t psa_hash_update(psa_hash_operation_t *operation, const uint8_t *bytes, size_t size)
{ for (size_t i = 0; i < size; ++i) operation->sum += bytes[i]; return PSA_SUCCESS; }
psa_status_t psa_hash_finish(psa_hash_operation_t *operation, uint8_t *output, size_t capacity, size_t *size)
{ assert(capacity == 32); for (size_t i = 0; i < capacity; ++i) output[i] = (uint8_t)(operation->sum + i); *size = capacity; return PSA_SUCCESS; }
psa_status_t psa_hash_abort(psa_hash_operation_t *operation) { (void)operation; return PSA_SUCCESS; }

int main(void)
{
    esp_base_ota_receipt_view_t view;
    reset();
    esp_base_ota_update_request_t ota = request(OP);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_NOT_FOUND);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(writes == 1 && commits == 1 && reads == 1 && handles == 0);
    assert(esp_base_ota_receipt_query(DEVICE, OP, true, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_RUNNING && !view.error_code && partition_reads == 0);
    assert(!strcmp(view.operation_id, OP) && view.image_size_bytes == IMAGE_BYTES && view.target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
    assert(!memcmp(view.sha256, ota.sha256, 32));
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN && partition_reads == 0);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_EXISTS && writes == 1);
    ota.sha256[0] ^= 1;
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_CONFLICT && writes == 1);
    ota.sha256[0] ^= 1;
    esp_base_ota_update_request_t next = request(NEXT_OP);
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_BUSY && writes == 1);

    reset(); ota = request(OP); next = request(NEXT_OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);

    running = boot = &new_slot; source_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_RUNNING);
    source_state = ESP_OTA_IMG_VALID;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_SUCCEEDED && !view.error_code && partition_reads == 1);
    image[0] ^= 1;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN);
    image[0] ^= 1;
    target_lookup = ESP_OK; target_state = ESP_OTA_IMG_VALID;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_OK && writes == 2);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_NOT_FOUND);

    reset(); ota = request(OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    target_lookup = ESP_OK; target_state = ESP_OTA_IMG_ABORTED;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_FAILED && !strcmp(view.error_code, "ota_rolled_back"));
    reset(); ota = request(OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_FAILED && !strcmp(view.error_code, "ota_download_failed"));
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_OK);

    reset(); ota = request(OP); next = request(NEXT_OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    target_lookup = ESP_OK; target_state = ESP_OTA_IMG_NEW;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE && writes == 1);
    target_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE && writes == 1);
    target_lookup = ESP_FAIL;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN && writes == 1);
    target_lookup = ESP_OK; target_state = ESP_OTA_IMG_VALID;
    boot = &new_slot;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH && writes == 1);
    boot = &old_slot; source_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID && writes == 1);
    source_state = ESP_OTA_IMG_VALID;
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_BUSY && writes == 1);

    reset(); ota = request(OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    fault = COMMIT_FAULT;
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    fault = NO_FAULT;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN);
    fault = SET_AFTER_FAULT;
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    fault = NO_FAULT;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_FAILED); /* Error may follow a durable write. */
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, ESP_BASE_OTA_UPDATE_BOOT_STATE_UNKNOWN) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE);

    reset(); ota = request(OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    stored[4] = 9;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    assert(esp_base_ota_receipt_register(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);

    for (int scenario = INIT_FAULT; scenario <= READBACK_MISMATCH; ++scenario) {
        reset(); ota = request(OP);
        if (scenario == READ_FAULT) {
            assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
            ota = request(NEXT_OP);
        }
        fault = scenario;
        const esp_base_ota_receipt_result_t result = esp_base_ota_receipt_register(DEVICE, &ota);
        if (scenario == OPEN_WRITE_FAULT) assert(result == ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE);
        else assert(result == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
        assert(handles == 0);
    }
    reset(); signed_enabled = false; ota = request(OP);
    assert(esp_base_ota_receipt_register(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_UNSUPPORTED && writes == 0);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_UNSUPPORTED && reads == 0);
    puts("  ota_receipt passed (durable intent, active/pending/valid/rollback, uncertain NVS; fake SDK)");
}
