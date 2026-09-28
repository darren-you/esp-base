#include "esp_base_ota_receipt.h"

#include <stddef.h>
#include <string.h>

#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

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
    if (candidate.image_size_bytes == 0 ||
        candidate.image_size_bytes > esp_base_ota_policy(false).ota_size_bytes ||
        (candidate.status == OTA_STATUS_PREPARED && candidate.failure != 0) ||
        (candidate.status == OTA_STATUS_FAILED &&
         (candidate.failure == EOTA_UPDATE_OK ||
          candidate.failure == EOTA_UPDATE_BOOT_STATE_UNKNOWN ||
          candidate.failure > EOTA_UPDATE_RESOURCE_FAILURE))) return false;
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

static void evaluate(const receipt_t *receipt, bool worker_active, esp_base_ota_receipt_view_t *view)
{
    view->state = ESP_BASE_OTA_OPERATION_UNKNOWN;
    view->error_code = "ota_result_uncertain";
    if (worker_active) { view->state = ESP_BASE_OTA_OPERATION_RUNNING; view->error_code = NULL; return; }
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots;
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK) return;
    if (slots.running_subtype == receipt->target_subtype &&
        slots.boot_subtype == receipt->target_subtype) {
        if (slots.running_state == EOTA_STATE_PENDING_VERIFY) {
            view->state = ESP_BASE_OTA_OPERATION_RUNNING;
            view->error_code = NULL;
        } else if (slots.running_state == EOTA_STATE_VALID) {
            uint8_t digest[EOTA_SHA256_BYTES];
            if (eota_sha256_running(&policy, receipt->image_size_bytes, digest) == EOTA_UPDATE_OK &&
                memcmp(digest, receipt->sha256, sizeof digest) == 0) {
                view->state = ESP_BASE_OTA_OPERATION_SUCCEEDED;
                view->error_code = NULL;
            }
        }
        return;
    }
    if (slots.running_subtype != receipt->source_subtype ||
        slots.boot_subtype != receipt->source_subtype || slots.running_state != EOTA_STATE_VALID) return;
    if (receipt->status == OTA_STATUS_FAILED) {
        view->state = ESP_BASE_OTA_OPERATION_FAILED;
        view->error_code = eota_error((eota_result_t)receipt->failure);
        return;
    }
    if (slots.target_subtype == receipt->target_subtype &&
        (slots.target_state == EOTA_STATE_ABORTED || slots.target_state == EOTA_STATE_INVALID)) {
        view->state = ESP_BASE_OTA_OPERATION_FAILED;
        view->error_code = "ota_rolled_back";
    }
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
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
    const char *device_id, const esp_base_ota_request_t *request)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || request == NULL || !valid_uuid(request->operation_id)) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    }
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots;
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK) {
        return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    }
    if (request->image_size_bytes == 0 || request->image_size_bytes > slots.target_size_bytes) {
        return ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    }
    if (slots.running_subtype != slots.boot_subtype ||
        slots.running_address_bytes != slots.boot_address_bytes) return ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH;
    if (slots.running_state != EOTA_STATE_VALID) return ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID;
    if (slots.target_state == EOTA_STATE_NEW || slots.target_state == EOTA_STATE_PENDING_VERIFY) {
        return ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE;
    }
    if (slots.target_state != EOTA_STATE_UNTRACKED && slots.target_state != EOTA_STATE_UNDEFINED &&
        slots.target_state != EOTA_STATE_VALID && slots.target_state != EOTA_STATE_INVALID &&
        slots.target_state != EOTA_STATE_ABORTED) return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    if (eota_preflight(&policy, request->image_size_bytes, &slots) != EOTA_UPDATE_OK) {
        return ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
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
    receipt_t next = {.status = OTA_STATUS_PREPARED, .source_subtype = slots.running_subtype,
        .target_subtype = slots.target_subtype, .image_size_bytes = request->image_size_bytes};
    memcpy(next.device_id, device_id, sizeof next.device_id);
    memcpy(next.operation_id, request->operation_id, sizeof next.operation_id);
    memcpy(next.sha256, request->sha256, sizeof next.sha256);
    return store(&next);
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || !valid_uuid(operation_id) || error == EOTA_UPDATE_OK ||
        error == EOTA_UPDATE_BOOT_STATE_UNKNOWN ||
        error > EOTA_UPDATE_RESOURCE_FAILURE) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
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
