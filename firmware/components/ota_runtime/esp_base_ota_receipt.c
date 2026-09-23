#include "esp_base_ota_receipt.h"

#include <stddef.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#define OTA_PARTITION "base_store"
#define OTA_NAMESPACE "base_ota"
#define OTA_KEY "operation"
#define OTA_BYTES 118
#define OTA_STATUS_PREPARED 1
#define OTA_STATUS_FAILED 2

typedef struct {
    uint8_t status, source_subtype, target_subtype, failure;
    uint32_t image_size_bytes;
    char device_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint8_t sha256[32];
} receipt_t;

static bool valid_uuid(const char *value)
{
    if (value == NULL || strnlen(value, ESP_BASE_OTA_OPERATION_ID_BYTES) != 36 ||
        value[14] != '4' || strchr("89ab", value[19]) == NULL) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

static bool valid_slot(const esp_partition_t *partition)
{
    if (partition == NULL || partition->type != ESP_PARTITION_TYPE_APP || partition->size != 0x1e0000) return false;
    return (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && partition->address == 0x20000) ||
           (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && partition->address == 0x200000);
}

static bool same_slot(const esp_partition_t *partition, uint8_t subtype)
{
    return valid_slot(partition) && partition->subtype == subtype;
}

static void encode(const receipt_t *receipt, uint8_t bytes[OTA_BYTES])
{
    memcpy(bytes, "EOTA", 4);
    bytes[4] = 1;
    bytes[5] = receipt->status;
    bytes[6] = receipt->source_subtype;
    bytes[7] = receipt->target_subtype;
    bytes[8] = receipt->failure;
    bytes[9] = 0;
    for (unsigned i = 0; i < 4; ++i) bytes[10 + i] = (uint8_t)(receipt->image_size_bytes >> (8 * i));
    memcpy(bytes + 14, receipt->device_id, 36);
    memcpy(bytes + 50, receipt->operation_id, 36);
    memcpy(bytes + 86, receipt->sha256, 32);
}

static bool decode(const uint8_t bytes[OTA_BYTES], receipt_t *receipt)
{
    if (memcmp(bytes, "EOTA", 4) || bytes[4] != 1 || bytes[9] != 0 ||
        (bytes[5] != OTA_STATUS_PREPARED && bytes[5] != OTA_STATUS_FAILED) ||
        !((bytes[6] == ESP_PARTITION_SUBTYPE_APP_OTA_0 && bytes[7] == ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
          (bytes[6] == ESP_PARTITION_SUBTYPE_APP_OTA_1 && bytes[7] == ESP_PARTITION_SUBTYPE_APP_OTA_0))) return false;
    receipt_t candidate = {0};
    candidate.status = bytes[5];
    candidate.source_subtype = bytes[6];
    candidate.target_subtype = bytes[7];
    candidate.failure = bytes[8];
    for (unsigned i = 0; i < 4; ++i) candidate.image_size_bytes |= (uint32_t)bytes[10 + i] << (8 * i);
    if (candidate.image_size_bytes == 0 || candidate.image_size_bytes > 0x1e0000 ||
        (candidate.status == OTA_STATUS_PREPARED && candidate.failure != 0) ||
        (candidate.status == OTA_STATUS_FAILED &&
         (candidate.failure == ESP_BASE_OTA_UPDATE_OK ||
          candidate.failure == ESP_BASE_OTA_UPDATE_BOOT_STATE_UNKNOWN ||
          candidate.failure > ESP_BASE_OTA_UPDATE_RESOURCE_FAILURE))) return false;
    memcpy(candidate.device_id, bytes + 14, 36);
    memcpy(candidate.operation_id, bytes + 50, 36);
    memcpy(candidate.sha256, bytes + 86, 32);
    if (!valid_uuid(candidate.device_id) || !valid_uuid(candidate.operation_id)) return false;
    *receipt = candidate;
    return true;
}

static esp_base_ota_receipt_result_t load(receipt_t *receipt)
{
    esp_err_t error = nvs_flash_init_partition(OTA_PARTITION);
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    nvs_handle_t handle;
    error = nvs_open_from_partition(OTA_PARTITION, OTA_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    uint8_t bytes[OTA_BYTES];
    size_t size = sizeof bytes;
    error = nvs_get_blob(handle, OTA_KEY, bytes, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    if (error != ESP_OK || size != sizeof bytes || !decode(bytes, receipt)) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    }
    return ESP_BASE_OTA_RECEIPT_OK;
}

static esp_base_ota_receipt_result_t store(const receipt_t *receipt)
{
    uint8_t bytes[OTA_BYTES];
    encode(receipt, bytes);
    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition(OTA_PARTITION, OTA_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    error = nvs_set_blob(handle, OTA_KEY, bytes, sizeof bytes);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    receipt_t observed;
    if (load(&observed) != ESP_BASE_OTA_RECEIPT_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    uint8_t actual[OTA_BYTES];
    encode(&observed, actual);
    return memcmp(bytes, actual, sizeof bytes) == 0 ? ESP_BASE_OTA_RECEIPT_OK :
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
}

static bool running_digest_matches(const esp_partition_t *partition, const receipt_t *receipt)
{
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    uint8_t buffer[1024], digest[32];
    for (uint32_t offset = 0; offset < receipt->image_size_bytes;) {
        const size_t count = receipt->image_size_bytes - offset < sizeof buffer ?
            receipt->image_size_bytes - offset : sizeof buffer;
        if (esp_partition_read(partition, offset, buffer, count) != ESP_OK ||
            psa_hash_update(&hash, buffer, count) != PSA_SUCCESS) {
            (void)psa_hash_abort(&hash);
            return false;
        }
        offset += (uint32_t)count;
    }
    size_t size = 0;
    if (psa_hash_finish(&hash, digest, sizeof digest, &size) != PSA_SUCCESS || size != sizeof digest) {
        return false;
    }
    return memcmp(digest, receipt->sha256, sizeof digest) == 0;
}

static void evaluate(const receipt_t *receipt, bool worker_active, esp_base_ota_receipt_view_t *view)
{
    view->state = ESP_BASE_OTA_OPERATION_UNKNOWN;
    view->error_code = "ota_result_uncertain";
    if (worker_active) { view->state = ESP_BASE_OTA_OPERATION_RUNNING; view->error_code = NULL; return; }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (same_slot(running, receipt->target_subtype) && same_slot(boot, receipt->target_subtype)) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            view->state = ESP_BASE_OTA_OPERATION_RUNNING;
            view->error_code = NULL;
        } else if (state == ESP_OTA_IMG_VALID && running_digest_matches(running, receipt)) {
            view->state = ESP_BASE_OTA_OPERATION_SUCCEEDED;
            view->error_code = NULL;
        }
        return;
    }
    if (!same_slot(running, receipt->source_subtype) || !same_slot(boot, receipt->source_subtype)) return;
    esp_ota_img_states_t source_state;
    if (esp_ota_get_state_partition(running, &source_state) != ESP_OK || source_state != ESP_OTA_IMG_VALID) return;
    if (receipt->status == OTA_STATUS_FAILED) {
        view->state = ESP_BASE_OTA_OPERATION_FAILED;
        view->error_code = esp_base_ota_update_error((esp_base_ota_update_result_t)receipt->failure);
        return;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t target_state;
    if (same_slot(target, receipt->target_subtype) &&
        esp_ota_get_state_partition(target, &target_state) == ESP_OK &&
        (target_state == ESP_OTA_IMG_ABORTED || target_state == ESP_OTA_IMG_INVALID)) {
        view->state = ESP_BASE_OTA_OPERATION_FAILED;
        view->error_code = "ota_rolled_back";
    }
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    if (!esp_base_ota_update_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!view || !valid_uuid(device_id) || !valid_uuid(operation_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    *view = (esp_base_ota_receipt_view_t){0};
    receipt_t receipt;
    const esp_base_ota_receipt_result_t result = load(&receipt);
    if (result != ESP_BASE_OTA_RECEIPT_OK) return result;
    if (strcmp(receipt.device_id, device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    if (strcmp(receipt.operation_id, operation_id)) return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    memcpy(view->operation_id, receipt.operation_id, sizeof view->operation_id);
    memcpy(view->sha256, receipt.sha256, sizeof view->sha256);
    view->image_size_bytes = receipt.image_size_bytes;
    view->target_subtype = receipt.target_subtype;
    evaluate(&receipt, worker_active, view);
    return ESP_BASE_OTA_RECEIPT_OK;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_update_request_t *request)
{
    if (!esp_base_ota_update_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || request == NULL || !valid_uuid(request->operation_id)) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!valid_slot(running) || !valid_slot(boot) || !valid_slot(target) ||
        running->subtype == target->subtype ||
        request->image_size_bytes == 0 || request->image_size_bytes > target->size) {
        return ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    }
    if (running->subtype != boot->subtype) return ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_VALID) {
        return ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID;
    }
    const esp_err_t target_error = esp_ota_get_state_partition(target, &state);
    if (target_error == ESP_OK) {
        if (state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY) {
            return ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE;
        }
        if (state != ESP_OTA_IMG_UNDEFINED && state != ESP_OTA_IMG_VALID &&
            state != ESP_OTA_IMG_INVALID && state != ESP_OTA_IMG_ABORTED) {
            return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
        }
    } else if (target_error != ESP_ERR_NOT_FOUND) {
        return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    }
    receipt_t previous;
    esp_base_ota_receipt_result_t existing = load(&previous);
    if (existing != ESP_BASE_OTA_RECEIPT_NOT_FOUND && existing != ESP_BASE_OTA_RECEIPT_OK) return existing;
    if (existing == ESP_BASE_OTA_RECEIPT_OK) {
        if (strcmp(previous.device_id, device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
        if (!strcmp(previous.operation_id, request->operation_id)) {
            return previous.image_size_bytes == request->image_size_bytes &&
                   memcmp(previous.sha256, request->sha256, 32) == 0 ?
                   ESP_BASE_OTA_RECEIPT_EXISTS : ESP_BASE_OTA_RECEIPT_CONFLICT;
        }
        esp_base_ota_receipt_view_t view = {0};
        evaluate(&previous, false, &view);
        if (view.state != ESP_BASE_OTA_OPERATION_SUCCEEDED && view.state != ESP_BASE_OTA_OPERATION_FAILED) {
            return ESP_BASE_OTA_RECEIPT_BUSY;
        }
    }
    receipt_t next = {.status = OTA_STATUS_PREPARED, .source_subtype = running->subtype,
        .target_subtype = target->subtype, .image_size_bytes = request->image_size_bytes};
    memcpy(next.device_id, device_id, sizeof next.device_id);
    memcpy(next.operation_id, request->operation_id, sizeof next.operation_id);
    memcpy(next.sha256, request->sha256, sizeof next.sha256);
    return store(&next);
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, esp_base_ota_update_result_t error)
{
    if (!esp_base_ota_update_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || !valid_uuid(operation_id) || error == ESP_BASE_OTA_UPDATE_OK ||
        error == ESP_BASE_OTA_UPDATE_BOOT_STATE_UNKNOWN ||
        error > ESP_BASE_OTA_UPDATE_RESOURCE_FAILURE) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    receipt_t receipt;
    esp_base_ota_receipt_result_t result = load(&receipt);
    if (result != ESP_BASE_OTA_RECEIPT_OK) return result;
    if (strcmp(receipt.device_id, device_id) || strcmp(receipt.operation_id, operation_id)) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    }
    receipt.status = OTA_STATUS_FAILED;
    receipt.failure = error;
    return store(&receipt);
}
