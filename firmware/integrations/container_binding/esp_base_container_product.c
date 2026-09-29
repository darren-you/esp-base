// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_product.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_base_container_binding.h"
#include "esp_base_container_no_package.h"
#include "esp_container_package_slot.h"
#include "esp_container_product.h"
#include "esp_container_slots.h"
#include "esp_container_slots_idf.h"

static const char *TAG = "base_container";

/* After retirement, success persists stage, begin_trial, mark_healthy and
 * confirm. A rollback from HEALTH_VERIFIED instead persists abandon and drop,
 * requiring five commits after retirement. A/B adds the retirement commit. */
enum {
    ESP_BASE_CONTAINER_OTA_CONFIRM_COMMITS = 4U,
    ESP_BASE_CONTAINER_OTA_RECOVERY_COMMITS = 5U,
};

typedef struct {
    size_t size_bytes;
    uint64_t event_sequence;
    uint8_t event_sha256[32];
    uint8_t bytes[];
} product_event_t;

typedef struct {
    econtainer_slots_idf_provider_t provider;
    econtainer_package_slot_validation_t validation;
    econtainer_runtime_limits_t limits;
    uint8_t public_key[512];
    size_t public_key_size_bytes;
    SemaphoreHandle_t ready;
    SemaphoreHandle_t stopped;
    SemaphoreHandle_t storage_lock;
    SemaphoreHandle_t event_lock;
    esp_base_storage_owner_t *flash_io_owner;
    esp_base_storage_claim_t flash_io_claim;
    const esp_base_storage_claim_t *claim;
    pthread_t thread;
    bool thread_joinable;
    bool start_attempted;
    bool reopen_allowed;
    bool uninstall_uncertain;
    atomic_int result;
    bool trial_mode;
    bool package_trial_mode;
    uint32_t package_trial_sequence;
    uint8_t package_trial_operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
    atomic_bool stop_requested;
    atomic_bool instance_active;
    atomic_bool boot_admitted;
    atomic_bool event_accepting;
    atomic_uint_fast32_t event_progress_count;
    product_event_t **event_queue;
    size_t event_capacity;
    size_t event_head;
    size_t event_count;
    bool guest_call_processing;
    bool trial_commit_active;
    uint8_t event_package_sha256[32];
    uint8_t trial_event_sha256[32];
    uint64_t representative_event_sequence;
    uint64_t trial_failure_count;
    esp_base_container_event_observation_t last_event_observation;
    bool stop_succeeded;
    /* Native runtime was absent or stop/close completed before worker join.
     * A failed guest can still be safely unbound after this proof. */
    bool native_reclaimed;
    bool provider_bound;
} product_context_t;

static product_context_t s_product;

void esp_base_container_product_set_flash_io_owner(esp_base_storage_owner_t *owner)
{
    if (s_product.provider_bound ||
        esp_base_storage_claim_active(&s_product.flash_io_claim)) return;
    s_product.flash_io_owner = owner;
}

static bool acquire_flash_io(void *context)
{
    product_context_t *product = context;
    return product != NULL && product->flash_io_owner != NULL &&
        esp_base_storage_claim(product->flash_io_owner, &product->flash_io_claim);
}

static bool release_flash_io(void *context)
{
    product_context_t *product = context;
    const bool released = product != NULL &&
        esp_base_storage_release(&product->flash_io_claim);
    if (!released) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED short Flash I/O lease release failed");
    }
    return released;
}

/* Every field is an independently approved build input. An entirely empty
 * configuration is the existing no-package Base product; any partial input
 * activates a strict reject path rather than silently ignoring it. */
static bool policy_present(void)
{
    return CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_KEY_ID[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PUBLIC_KEY_DER_HEX[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_LABEL[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES != 0;
}

bool esp_base_container_product_configured(void)
{
    return policy_present();
}

bool esp_base_container_product_ota_ready(void)
{
    return !policy_present() ||
        (atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire) &&
         atomic_load_explicit(&s_product.result, memory_order_acquire) ==
             ESP_BASE_CONTAINER_EMPTY);
}

static bool snapshot_mode_ready(esp_base_ota_package_mode_t package_mode)
{
    if (package_mode != ESP_BASE_OTA_NO_PACKAGE &&
        package_mode != ESP_BASE_OTA_PACKAGE_REUSE &&
        package_mode != ESP_BASE_OTA_PACKAGE_WRITE) return false;
    if (package_mode == ESP_BASE_OTA_NO_PACKAGE)
        return esp_base_container_product_ota_ready();
    if (!policy_present() || !s_product.provider_bound ||
        !atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire) ||
        s_product.trial_mode || s_product.package_trial_mode) return false;
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    const bool active = atomic_load_explicit(&s_product.instance_active,
                                              memory_order_acquire);
    if (result == ESP_BASE_CONTAINER_EMPTY)
        return package_mode == ESP_BASE_OTA_PACKAGE_WRITE && !active;
    return result == ESP_BASE_CONTAINER_RUNNING && active &&
        atomic_load_explicit(&s_product.event_accepting, memory_order_acquire) &&
        !atomic_load_explicit(&s_product.stop_requested, memory_order_acquire);
}

static int hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static bool decode_public_key(void)
{
    const char *hex = CONFIG_ESP_BASE_CONTAINER_PUBLIC_KEY_DER_HEX;
    const size_t length = strlen(hex);
    if (length == 0 || (length & 1U) != 0U || length > sizeof(s_product.public_key) * 2U) {
        return false;
    }
    for (size_t index = 0; index < length / 2U; ++index) {
        const int upper = hex_digit(hex[index * 2U]);
        const int lower = hex_digit(hex[index * 2U + 1U]);
        if (upper < 0 || lower < 0) return false;
        s_product.public_key[index] = (uint8_t)((upper << 4) | lower);
    }
    s_product.public_key_size_bytes = length / 2U;
    return true;
}

static bool decode_uuid(const char value[37], uint8_t bytes[16])
{
    if (value == NULL || strnlen(value, 37) != 36 ||
        value[14] != '4' || strchr("89ab", value[19]) == NULL) return false;
    size_t written = 0;
    for (size_t index = 0; index < 36; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
            continue;
        }
        const int upper = hex_digit(value[index]);
        if (upper < 0 || ++index == 36) return false;
        const int lower = hex_digit(value[index]);
        if (lower < 0 || written >= 16) return false;
        bytes[written++] = (uint8_t)((upper << 4) | lower);
    }
    return written == 16;
}

static bool configure_policy(void)
{
    if (CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_KEY_ID[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_NVS_LABEL[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES > ECONTAINER_PACKAGE_WASM_MAX_BYTES ||
        CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES > 65536 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS > INT32_MAX ||
        CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES < 16384 ||
        (CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES & ~ECONTAINER_CAP_ALL) != 0 ||
        ((CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES & ECONTAINER_CAP_TIMER) != 0 ?
             CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS <= 0 :
             CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS != 0) ||
        ((CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES & ECONTAINER_CAP_LOG) != 0 ?
             CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES <= 0 ||
             CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES > 256 :
             CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES != 0) ||
        !decode_public_key()) return false;

    s_product.validation = (econtainer_package_slot_validation_t){
        .expected_product_id = CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID,
        .public_key_rsa_der = s_product.public_key,
        .public_key_size_bytes = s_product.public_key_size_bytes,
        .expected_key_id = CONFIG_ESP_BASE_CONTAINER_KEY_ID,
        .max_wasm_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES,
        .wasm_authorization = {
            .allowed_capabilities = CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES,
            .max_memory_bytes = 65536U,
            .max_stack_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES,
        },
        .max_event_queue_limit = CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE,
        .max_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .max_host_call_timeout_ms = CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS,
        .max_storage_limit_bytes = 0U,
    };
    s_product.limits = (econtainer_runtime_limits_t){
        .max_wasm_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES,
        .max_memory_pages = 1U,
        .stack_size_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES,
        .max_event_bytes = ECONTAINER_EVENT_BUFFER_BYTES,
        .allowed_capabilities = CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES,
        .max_log_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES,
        .max_timers = CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS,
        .init_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .event_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .stop_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .max_entry_duration_ms = CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS,
    };
    return true;
}

typedef struct {
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
} stage_context_t;

static econtainer_slots_result_t stage_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    if (firmware_set->bootable_count != 2U) return ECONTAINER_SLOTS_CONFLICT;
    econtainer_slots_state_t current = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &current);
    if (result != ECONTAINER_SLOTS_OK) return result;

    const econtainer_slot_binding_t *running = NULL;
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (current.bindings[index].present &&
            memcmp(current.bindings[index].firmware_sha256,
                   firmware_set->running_firmware_sha256, 32) == 0) {
            running = &current.bindings[index];
            break;
        }
    }
    if (running == NULL) return ECONTAINER_SLOTS_CONFLICT;
    /* The product-only event predicate does not authorize a firmware trial.
     * REUSE and WRITE need their receipt-bound startup/health path first. */
    if (running->package_present) return ECONTAINER_SLOTS_UNTRUSTED;

    const stage_context_t *stage = context;
    econtainer_slot_operation_t operation = {0};
    memcpy(operation.operation_id, stage->operation_id, sizeof operation.operation_id);
    memcpy(operation.target_firmware_sha256,
           firmware_set->bootable_firmware_sha256[1], 32);
    operation.kind = ECONTAINER_SLOT_NO_PACKAGE;
    return econtainer_slots_stage_firmware(
        &s_product.provider.io, &s_product.provider.geometry, current.sequence,
        firmware_set, &operation, NULL, NULL, &current);
}

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES])
{
    if (!policy_present()) return ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED;
    if (!esp_base_storage_claim_active(claim) || prepared == NULL ||
        s_product.ready == NULL ||
        !atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire)) {
        return ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
    }
    stage_context_t stage = {0};
    if (!decode_uuid(operation_id, stage.operation_id)) {
        return ESP_BASE_CONTAINER_STAGE_REJECTED;
    }
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, prepared,
        stage_firmware, &stage);
    if (result == ECONTAINER_SLOTS_OK) return ESP_BASE_CONTAINER_STAGE_PREPARED;
    if (result == ECONTAINER_SLOTS_CONFLICT || result == ECONTAINER_SLOTS_UNTRUSTED ||
        result == ECONTAINER_SLOTS_EMPTY || result == ECONTAINER_SLOTS_NO_SPACE) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STAGE_REJECTED result=%d", (int)result);
        return ESP_BASE_CONTAINER_STAGE_REJECTED;
    }
    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STAGE_UNCERTAIN result=%d", (int)result);
    return ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
}

typedef struct {
    uint32_t sequence;
    bool trial;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
    econtainer_runtime_t *runtime;
    econtainer_runtime_result_t runtime_result;
    uint32_t event_queue_limit;
    uint8_t package_sha256[32];
} open_context_t;

static econtainer_slots_result_t open_selected(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    open_context_t *open = context;
    /* Product open consumes these only while this pthread call is active.
     * The Container's selected-slot path supplies its own verified_info. */
    econtainer_package_workspace_t package_workspace;
    econtainer_wasm_workspace_t wasm_workspace;
    econtainer_package_slot_validation_t validation = s_product.validation;
    validation.package_workspace = &package_workspace;
    validation.wasm_workspace = &wasm_workspace;
    econtainer_slot_selection_request_t request = {
        .expected_sequence = open->sequence,
        .firmware_set = *firmware_set,
        .selection = open->trial ? ECONTAINER_SLOT_SELECT_TRIAL :
                                   ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    if (open->trial) {
        memcpy(request.operation_id, open->operation_id, sizeof request.operation_id);
        memcpy(request.boot_id, open->boot_id, sizeof request.boot_id);
    }
    const econtainer_slot_runtime_result_t result = econtainer_product_open(
        &s_product.provider.io, &s_product.provider.geometry, &request,
        &validation, &s_product.limits, &open->runtime);
    open->runtime_result = result.runtime;
    if (result.slots == ECONTAINER_SLOTS_OK &&
        result.runtime == ECONTAINER_RUNTIME_OK && open->runtime != NULL) {
        open->event_queue_limit = result.event_queue_limit;
        memcpy(open->package_sha256, result.package_sha256,
               sizeof open->package_sha256);
    }
    return result.slots;
}

static void report_result(esp_base_container_boot_result_t result)
{
    atomic_store_explicit(&s_product.result, result, memory_order_release);
    xSemaphoreGive(s_product.ready);
}

typedef struct {
    uint32_t expected_sequence;
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
    econtainer_slots_state_t state;
} begin_trial_context_t;

static econtainer_slots_result_t begin_trial(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    begin_trial_context_t *begin = context;
    return econtainer_slots_begin_trial(
        &s_product.provider.io, &s_product.provider.geometry,
        begin->expected_sequence, firmware_set->running_firmware_sha256,
        begin->boot_id, &begin->state);
}

static econtainer_slots_result_t initialize_no_package(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)context;
    bool initialized = false;
    const econtainer_slots_result_t result = esp_base_container_initialize_no_package(
        &s_product.provider.io, &s_product.provider.geometry, firmware_set, &initialized);
    if (initialized) {
        ESP_LOGI(TAG, "ESP_BASE_CONTAINER_INITIALIZED_NO_PACKAGE firmware_count=%u",
                 (unsigned)firmware_set->bootable_count);
    }
    return result;
}

static bool drain_log(econtainer_runtime_t *runtime)
{
    uint8_t bytes[256];
    size_t size_bytes = 0;
    const econtainer_runtime_result_t result = econtainer_product_take_log(
        runtime, bytes, sizeof bytes, &size_bytes);
    if (result == ECONTAINER_RUNTIME_NO_LOG) return true;
    if (result != ECONTAINER_RUNTIME_OK || size_bytes > sizeof bytes) return false;
    char hex[sizeof bytes * 2U + 1U];
    const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size_bytes; ++index) {
        hex[index * 2U] = digits[bytes[index] >> 4];
        hex[index * 2U + 1U] = digits[bytes[index] & 15U];
    }
    hex[size_bytes * 2U] = '\0';
    ESP_LOGI(TAG, "ESP_BASE_CONTAINER_LOG_HEX bytes=%u data=%s",
             (unsigned)size_bytes, hex);
    return true;
}

static void release_event_queue(void)
{
    product_event_t **queue = NULL;
    size_t capacity = 0;
    if (s_product.event_lock != NULL &&
        xSemaphoreTake(s_product.event_lock, portMAX_DELAY) == pdTRUE) {
        atomic_store_explicit(&s_product.event_accepting, false,
                              memory_order_release);
        queue = s_product.event_queue;
        capacity = s_product.event_capacity;
        s_product.event_queue = NULL;
        s_product.event_capacity = 0;
        s_product.event_head = 0;
        s_product.event_count = 0;
        s_product.guest_call_processing = false;
        s_product.trial_commit_active = false;
        memset(s_product.event_package_sha256, 0,
               sizeof s_product.event_package_sha256);
        xSemaphoreGive(s_product.event_lock);
    }
    for (size_t index = 0; index < capacity; ++index) {
        if (queue[index] != NULL) {
            memset(queue[index]->bytes, 0, queue[index]->size_bytes);
            free(queue[index]);
        }
    }
    free(queue);
}

static product_event_t *take_event(void)
{
    product_event_t *event = NULL;
    if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE) return NULL;
    if (s_product.event_count != 0U) {
        event = s_product.event_queue[s_product.event_head];
        s_product.event_queue[s_product.event_head] = NULL;
        s_product.event_head = (s_product.event_head + 1U) % s_product.event_capacity;
        --s_product.event_count;
        s_product.guest_call_processing = true;
    }
    xSemaphoreGive(s_product.event_lock);
    return event;
}

static bool finish_guest_work(void)
{
    if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE) {
        atomic_store_explicit(&s_product.event_accepting, false,
                              memory_order_release);
        return false;
    }
    s_product.guest_call_processing = false;
    xSemaphoreGive(s_product.event_lock);
    return true;
}

static void fail_guest_work(void)
{
    atomic_store_explicit(&s_product.event_accepting, false,
                          memory_order_release);
    if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE) return;
    if (s_product.package_trial_mode) {
        if (s_product.trial_failure_count != UINT64_MAX)
            ++s_product.trial_failure_count;
        s_product.representative_event_sequence = 0U;
    }
    s_product.guest_call_processing = false;
    xSemaphoreGive(s_product.event_lock);
}

bool esp_base_container_product_event_accepting(void)
{
    return atomic_load_explicit(&s_product.event_accepting, memory_order_acquire) &&
        !atomic_load_explicit(&s_product.stop_requested, memory_order_acquire) &&
        atomic_load_explicit(&s_product.result, memory_order_acquire) ==
            ESP_BASE_CONTAINER_RUNNING;
}

uint32_t esp_base_container_product_event_progress_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_product.event_progress_count,
                                          memory_order_acquire);
}

esp_base_container_event_observation_result_t
esp_base_container_product_event_observation(
    esp_base_container_event_observation_t *out)
{
    if (out == NULL) return ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION;
    *out = (esp_base_container_event_observation_t){0};
    if (s_product.event_lock == NULL) return ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION;
    if (xSemaphoreTake(s_product.event_lock, 0U) != pdTRUE)
        return ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
    *out = s_product.last_event_observation;
    xSemaphoreGive(s_product.event_lock);
    return out->event_sequence == 0U ? ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION :
        ESP_BASE_CONTAINER_EVENT_OBSERVED;
}

bool esp_base_container_product_trial_event_snapshot(
    esp_base_container_trial_event_snapshot_t *out)
{
    if (out == NULL) return false;
    *out = (esp_base_container_trial_event_snapshot_t){0};
    if (s_product.event_lock == NULL ||
        xSemaphoreTake(s_product.event_lock, 0U) != pdTRUE) return false;
    const bool active = s_product.trial_mode && s_product.package_trial_mode &&
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire);
    if (active) {
        out->representative_event_sequence = s_product.representative_event_sequence;
        out->failure_count = s_product.trial_failure_count;
        memcpy(out->package_sha256, s_product.event_package_sha256, 32);
    }
    xSemaphoreGive(s_product.event_lock);
    return active;
}

esp_base_container_event_result_t esp_base_container_product_offer_event(
    const uint8_t package_sha256[32], uint64_t event_sequence,
    const uint8_t event_sha256[32], const uint8_t *event, size_t size_bytes)
{
    if (package_sha256 == NULL || event_sha256 == NULL || event_sequence == 0U ||
        event == NULL || size_bytes == 0U ||
        size_bytes > s_product.limits.max_event_bytes) {
        return ESP_BASE_CONTAINER_EVENT_INVALID;
    }
    if (!esp_base_container_product_event_accepting() ||
        s_product.event_lock == NULL) return ESP_BASE_CONTAINER_EVENT_UNAVAILABLE;
    product_event_t *copy = malloc(sizeof(*copy) + size_bytes);
    if (copy == NULL) return ESP_BASE_CONTAINER_EVENT_NO_MEMORY;
    copy->size_bytes = size_bytes;
    copy->event_sequence = event_sequence;
    memcpy(copy->event_sha256, event_sha256, sizeof copy->event_sha256);
    memcpy(copy->bytes, event, size_bytes);
    if (xSemaphoreTake(s_product.event_lock, 0U) != pdTRUE) {
        memset(copy->bytes, 0, size_bytes);
        free(copy);
        return ESP_BASE_CONTAINER_EVENT_BUSY;
    }
    esp_base_container_event_result_t result = ESP_BASE_CONTAINER_EVENT_ACCEPTED;
    if (!esp_base_container_product_event_accepting() ||
        s_product.event_queue == NULL) {
        result = ESP_BASE_CONTAINER_EVENT_UNAVAILABLE;
    } else if (memcmp(package_sha256, s_product.event_package_sha256, 32) != 0) {
        result = ESP_BASE_CONTAINER_EVENT_INVALID;
    } else if (s_product.event_count == s_product.event_capacity) {
        result = ESP_BASE_CONTAINER_EVENT_FULL;
    } else {
        const size_t tail = (s_product.event_head + s_product.event_count) %
                            s_product.event_capacity;
        s_product.event_queue[tail] = copy;
        ++s_product.event_count;
    }
    xSemaphoreGive(s_product.event_lock);
    if (result != ESP_BASE_CONTAINER_EVENT_ACCEPTED) {
        memset(copy->bytes, 0, size_bytes);
        free(copy);
    }
    return result;
}

static void *product_thread(void *unused)
{
    (void)unused;
    if (!s_product.trial_mode) {
        const econtainer_slots_result_t initialized = esp_base_container_with_firmware_set(
            s_product.claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
            initialize_no_package, NULL);
        if (initialized != ECONTAINER_SLOTS_OK) {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED initialization=%d", (int)initialized);
            s_product.stop_succeeded = true;
            s_product.native_reclaimed = true;
            xSemaphoreGive(s_product.stopped);
            report_result(ESP_BASE_CONTAINER_BLOCKED);
            return NULL;
        }
    }
    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const esp_base_ota_firmware_observation_t observation =
        s_product.trial_mode && !s_product.package_trial_mode ?
        ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL : ESP_BASE_OTA_FIRMWARE_CONFIRMED;
    const econtainer_slots_result_t reconcile = esp_base_container_reconcile(
        s_product.claim, observation,
        &s_product.provider.io, &s_product.provider.geometry, &state, &decision);
    if (reconcile == ECONTAINER_SLOTS_EMPTY) {
        s_product.stop_succeeded = true;
        s_product.native_reclaimed = true;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_EMPTY);
        return NULL;
    }
    const bool package_trial_ready = s_product.package_trial_mode &&
        decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED &&
        state.sequence == s_product.package_trial_sequence &&
        state.phase == ECONTAINER_SLOT_PREPARED &&
        state.operation.kind == ECONTAINER_SLOT_PACKAGE_WRITE &&
        !state.operation.firmware_transition &&
        memcmp(state.operation.operation_id, s_product.package_trial_operation_id,
               sizeof state.operation.operation_id) == 0;
    if (reconcile != ECONTAINER_SLOTS_OK ||
        (s_product.package_trial_mode ? !package_trial_ready :
         s_product.trial_mode ? decision != ECONTAINER_SLOT_BOOT_START_TRIAL :
         (decision != ECONTAINER_SLOT_BOOT_CONFIRMED &&
          decision != ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED))) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED reconcile=%d decision=%d",
                 (int)reconcile, (int)decision);
        s_product.stop_succeeded = true;
        s_product.native_reclaimed = true;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    if (s_product.trial_mode && !s_product.package_trial_mode &&
        state.operation.kind != ECONTAINER_SLOT_NO_PACKAGE) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED package trial lacks business event source");
        s_product.stop_succeeded = true;
        s_product.native_reclaimed = true;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }

    open_context_t open = {.sequence = state.sequence,
                           .trial = s_product.trial_mode,
                           .runtime_result = ECONTAINER_RUNTIME_INVALID_STATE};
    if (s_product.trial_mode) {
        begin_trial_context_t begin = {.expected_sequence = state.sequence};
        memcpy(begin.boot_id, s_product.boot_id, sizeof begin.boot_id);
        const econtainer_slots_result_t started = esp_base_container_with_firmware_set(
            s_product.claim, observation, NULL,
            begin_trial, &begin);
        if (started != ECONTAINER_SLOTS_OK) {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED begin_trial=%d", (int)started);
            s_product.stop_succeeded = true;
            s_product.native_reclaimed = true;
            xSemaphoreGive(s_product.stopped);
            report_result(ESP_BASE_CONTAINER_BLOCKED);
            return NULL;
        }
        open.sequence = begin.state.sequence;
        memcpy(open.operation_id, begin.state.operation.operation_id,
               sizeof open.operation_id);
        memcpy(open.boot_id, begin.boot_id, sizeof open.boot_id);
    }
    const econtainer_slots_result_t slots = esp_base_container_with_firmware_set(
        s_product.claim, observation, NULL, open_selected, &open);
    if (slots != ECONTAINER_SLOTS_OK ||
        open.runtime_result != ECONTAINER_RUNTIME_OK || open.runtime == NULL) {
        if (slots != ECONTAINER_SLOTS_EMPTY) {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED open_slots=%d open_runtime=%d",
                     (int)slots, (int)open.runtime_result);
        }
        const econtainer_runtime_result_t closed = open.runtime != NULL ?
            econtainer_product_close(&open.runtime) : ECONTAINER_RUNTIME_OK;
        s_product.native_reclaimed = closed == ECONTAINER_RUNTIME_OK && open.runtime == NULL;
        s_product.stop_succeeded = s_product.native_reclaimed;
        xSemaphoreGive(s_product.stopped);
        if (slots == ECONTAINER_SLOTS_EMPTY) {
            atomic_store_explicit(&s_product.boot_admitted, true, memory_order_release);
        }
        report_result(slots == ECONTAINER_SLOTS_EMPTY ? ESP_BASE_CONTAINER_EMPTY :
                      ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    const econtainer_runtime_result_t initialized = econtainer_product_init(open.runtime);
    if (initialized != ECONTAINER_RUNTIME_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED init=%d", (int)initialized);
        s_product.native_reclaimed = econtainer_product_close(&open.runtime) == ECONTAINER_RUNTIME_OK &&
                                     open.runtime == NULL;
        s_product.stop_succeeded = s_product.native_reclaimed;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    if (open.event_queue_limit == 0U ||
        open.event_queue_limit > s_product.validation.max_event_queue_limit ||
        s_product.event_lock == NULL) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED invalid signed event queue");
        const econtainer_runtime_result_t stopped = econtainer_product_stop(open.runtime);
        const econtainer_runtime_result_t closed = econtainer_product_close(&open.runtime);
        s_product.native_reclaimed = stopped == ECONTAINER_RUNTIME_OK &&
                                     closed == ECONTAINER_RUNTIME_OK && open.runtime == NULL;
        s_product.stop_succeeded = s_product.native_reclaimed;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    s_product.event_queue = calloc(open.event_queue_limit,
                                   sizeof(*s_product.event_queue));
    if (s_product.event_queue == NULL) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED event queue allocation");
        const econtainer_runtime_result_t stopped = econtainer_product_stop(open.runtime);
        const econtainer_runtime_result_t closed = econtainer_product_close(&open.runtime);
        s_product.native_reclaimed = stopped == ECONTAINER_RUNTIME_OK &&
                                     closed == ECONTAINER_RUNTIME_OK && open.runtime == NULL;
        s_product.stop_succeeded = s_product.native_reclaimed;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    s_product.event_capacity = open.event_queue_limit;
    s_product.event_head = 0;
    s_product.event_count = 0;
    memcpy(s_product.event_package_sha256, open.package_sha256,
           sizeof s_product.event_package_sha256);
    ESP_LOGI(TAG, "ESP_BASE_CONTAINER_RUNNING sequence=%u trial=%d",
             (unsigned)open.sequence, s_product.trial_mode);
    atomic_store_explicit(&s_product.boot_admitted, true, memory_order_release);
    atomic_store_explicit(&s_product.instance_active, true, memory_order_release);
    atomic_store_explicit(&s_product.event_accepting, true, memory_order_release);
    report_result(ESP_BASE_CONTAINER_RUNNING);

    bool requested_stop = false;
    for (;;) {
        if (atomic_load_explicit(&s_product.stop_requested, memory_order_acquire)) {
            requested_stop = true;
            break;
        }
        product_event_t *event = take_event();
        if (event != NULL) {
            int32_t guest_result = 0;
            const uint64_t event_sequence = event->event_sequence;
            uint8_t event_sha256[32];
            memcpy(event_sha256, event->event_sha256, sizeof event_sha256);
            const econtainer_runtime_result_t delivered = econtainer_product_on_event(
                open.runtime, event->bytes, event->size_bytes, &guest_result);
            memset(event->bytes, 0, event->size_bytes);
            free(event);
            if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE) {
                ESP_LOGE(TAG, "ESP_BASE_CONTAINER_EVENT_FAILED observation lock");
                break;
            }
            memcpy(s_product.last_event_observation.package_sha256,
                   s_product.event_package_sha256, 32);
            memcpy(s_product.last_event_observation.event_sha256,
                   event_sha256, 32);
            s_product.last_event_observation.event_sequence = event_sequence;
            s_product.last_event_observation.runtime_ok =
                delivered == ECONTAINER_RUNTIME_OK;
            s_product.last_event_observation.guest_result =
                delivered == ECONTAINER_RUNTIME_OK ? guest_result : 0;
            if (s_product.package_trial_mode) {
                if (delivered != ECONTAINER_RUNTIME_OK || guest_result < 0) {
                    if (s_product.trial_failure_count != UINT64_MAX)
                        ++s_product.trial_failure_count;
                    s_product.representative_event_sequence = 0U;
                } else if (memcmp(event_sha256, s_product.trial_event_sha256, 32) == 0) {
                    s_product.representative_event_sequence = event_sequence;
                }
            }
            s_product.guest_call_processing = false;
            xSemaphoreGive(s_product.event_lock);
            if (delivered != ECONTAINER_RUNTIME_OK) {
                ESP_LOGE(TAG, "ESP_BASE_CONTAINER_EVENT_FAILED result=%d", (int)delivered);
                break;
            }
            const uint32_t progress = esp_base_container_product_event_progress_count();
            if (progress != UINT32_MAX) {
                atomic_store_explicit(&s_product.event_progress_count, progress + 1U,
                                      memory_order_release);
            }
            continue;
        }
        if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE)
            break;
        const bool commit_active = s_product.trial_commit_active;
        if (!commit_active) s_product.guest_call_processing = true;
        xSemaphoreGive(s_product.event_lock);
        if (commit_active) {
            vTaskDelay(1U);
            continue;
        }
        if (!drain_log(open.runtime)) {
            fail_guest_work();
            break;
        }
        uint64_t deadline_ms = 0;
        const econtainer_runtime_result_t timer =
            econtainer_product_next_timer_deadline(open.runtime, &deadline_ms);
        if (timer == ECONTAINER_RUNTIME_OK) {
            const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
            if (deadline_ms <= now_ms) {
                econtainer_timer_event_t event = {0};
                int32_t guest_result = 0;
                const econtainer_runtime_result_t fired = econtainer_product_poll_timer(
                    open.runtime, &event, &guest_result);
                if (fired != ECONTAINER_RUNTIME_OK &&
                    fired != ECONTAINER_RUNTIME_NO_TIMER) {
                    fail_guest_work();
                    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_TIMER_FAILED result=%d",
                             (int)fired);
                    break;
                }
                if (!finish_guest_work()) break;
                continue;
            }
            const uint64_t wait_ms = deadline_ms - now_ms;
            if (!finish_guest_work()) break;
            vTaskDelay(pdMS_TO_TICKS(wait_ms > 1000U ? 1000U : (uint32_t)wait_ms));
        } else if (timer == ECONTAINER_RUNTIME_NO_TIMER) {
            if (!finish_guest_work()) break;
            vTaskDelay(pdMS_TO_TICKS(1000U));
        } else {
            fail_guest_work();
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_TIMER_FAILED result=%d", (int)timer);
            break;
        }
    }
    release_event_queue();
    const econtainer_runtime_result_t stopped = econtainer_product_stop(open.runtime);
    const econtainer_runtime_result_t closed = econtainer_product_close(&open.runtime);
    /* A trapped event/timer has already put the VM in FAILED. stop() then
     * reports INVALID_STATE, while close() is the native reclaim proof. */
    s_product.native_reclaimed =
        (stopped == ECONTAINER_RUNTIME_OK ||
         (!requested_stop && stopped == ECONTAINER_RUNTIME_INVALID_STATE)) &&
        closed == ECONTAINER_RUNTIME_OK && open.runtime == NULL;
    s_product.stop_succeeded = requested_stop && s_product.native_reclaimed;
    atomic_store_explicit(&s_product.instance_active, false, memory_order_release);
    if (!s_product.stop_succeeded) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED stopped=%d closed=%d",
                 (int)stopped, (int)closed);
    }
    atomic_store_explicit(&s_product.result,
                          s_product.stop_succeeded ? ESP_BASE_CONTAINER_STOPPED :
                          ESP_BASE_CONTAINER_BLOCKED,
                          memory_order_release);
    xSemaphoreGive(s_product.stopped);
    return NULL;
}

static bool ensure_provider(const esp_base_storage_claim_t *claim)
{
    if (!esp_base_storage_claim_active(claim) || !policy_present() ||
        s_product.flash_io_owner == NULL) return false;
    if (s_product.provider_bound) return true;
    /* A failed bind leaves this boot blocked. Do not replace a lock which may
     * already be referenced by the provider or retry with partial state. */
    if (s_product.storage_lock != NULL) return false;
    if (!configure_policy()) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED incomplete product authorization");
        return false;
    }
    s_product.storage_lock = xSemaphoreCreateMutex();
    if (s_product.storage_lock == NULL) return false;
    const econtainer_slots_idf_config_t config = {
        .package_partition_label = CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL,
        .package_partition_offset_bytes = CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET,
        .package_partition_size_bytes = CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE,
        .slots = {
            {CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET, CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE},
            {CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET, CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE},
            {CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET, CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE},
        },
        .nvs_partition_label = CONFIG_ESP_BASE_CONTAINER_NVS_LABEL,
        .nvs_partition_offset_bytes = CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET,
        .nvs_partition_size_bytes = CONFIG_ESP_BASE_CONTAINER_NVS_SIZE,
        .nvs_namespace = "base_pkg",
        .nvs_key = "slots",
        .storage_lock = s_product.storage_lock,
        .acquire_flash_io = acquire_flash_io,
        .release_flash_io = release_flash_io,
        .flash_io_context = &s_product,
    };
    if (!econtainer_slots_idf_bind(&s_product.provider, &config) ||
        !acquire_flash_io(&s_product)) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED unapproved partition or NVS");
        return false;
    }
    const esp_err_t initialized = nvs_flash_init_partition(CONFIG_ESP_BASE_CONTAINER_NVS_LABEL);
    const bool released = esp_base_storage_release(&s_product.flash_io_claim);
    if (initialized != ESP_OK || !released) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED NVS initialization or I/O lease");
        return false;
    }
    s_product.provider_bound = true;
    return true;
}

bool esp_base_container_product_without_ota_receipt(
    const esp_base_storage_claim_t *claim)
{
    if (!esp_base_storage_claim_active(claim)) return false;
    if (!policy_present()) return true;
    if (s_product.ready != NULL ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        !ensure_provider(claim)) return false;
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    return loaded == ECONTAINER_SLOTS_EMPTY ||
           (loaded == ECONTAINER_SLOTS_OK && !state.operation.firmware_transition);
}

static bool digest_zero(const uint8_t sha256[32])
{
    uint8_t value = 0;
    for (size_t index = 0; index < 32U; ++index) value |= sha256[index];
    return value == 0U;
}

/* Check the entire ECS2 identity set, not only a matching running entry.
 * The inactive confirmed package may exist at snapshot time; its reference
 * is validated by reconcile/retire before its binding is removed. */
static bool state_has_firmware(const econtainer_slots_state_t *state,
                               const uint8_t source_sha256[32],
                               const uint8_t other_sha256[32],
                               bool other_must_be_unpacked)
{
    bool source_found = false;
    bool other_found = false;
    const bool has_other = !digest_zero(other_sha256);
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        if (!binding->present) continue;
        if (memcmp(binding->firmware_sha256, source_sha256, 32) == 0) {
            if (source_found || binding->package_present) return false;
            source_found = true;
        } else if (has_other &&
                   memcmp(binding->firmware_sha256, other_sha256, 32) == 0) {
            if (other_found || (other_must_be_unpacked && binding->package_present))
                return false;
            other_found = true;
        } else {
            return false;
        }
    }
    return source_found && other_found == has_other;
}

static econtainer_slots_result_t verified_a_only(
    const econtainer_slot_firmware_set_t *firmware_set)
{
    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const econtainer_slots_result_t result = econtainer_slots_reconcile(
        &s_product.provider.io, &s_product.provider.geometry,
        firmware_set, &state, &decision);
    if (result != ECONTAINER_SLOTS_OK) return result;
    return state.phase == ECONTAINER_SLOT_IDLE &&
           decision == ECONTAINER_SLOT_BOOT_CONFIRMED &&
           state_has_firmware(&state, firmware_set->running_firmware_sha256,
                              (const uint8_t[32]){0}, false) ?
           ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}

typedef struct {
    bool container_enabled;
    esp_base_ota_package_mode_t package_mode;
    bool source_package_expected;
    esp_base_ota_receipt_snapshot_t *snapshot;
} snapshot_context_t;

static const econtainer_slot_binding_t *snapshot_running_binding(
    const econtainer_slots_state_t *state, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32])
{
    const econtainer_slot_binding_t *source = NULL;
    bool inactive_found = false;
    const bool has_inactive = !digest_zero(inactive_sha256);
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        if (!binding->present) continue;
        if (memcmp(binding->firmware_sha256, source_sha256, 32) == 0) {
            if (source != NULL) return NULL;
            source = binding;
        } else if (has_inactive &&
                   memcmp(binding->firmware_sha256, inactive_sha256, 32) == 0) {
            if (inactive_found) return NULL;
            inactive_found = true;
        } else return NULL;
    }
    return inactive_found == has_inactive ? source : NULL;
}

static econtainer_slots_result_t snapshot_for_ota(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    snapshot_context_t *entry = context;
    esp_base_ota_receipt_snapshot_t *snapshot = entry->snapshot;
    if (firmware_set->bootable_count < 1U || firmware_set->bootable_count > 2U)
        return ECONTAINER_SLOTS_CONFLICT;
    memcpy(snapshot->source_sha256, firmware_set->running_firmware_sha256, 32);
    if (firmware_set->bootable_count == 2U)
        memcpy(snapshot->inactive_sha256, firmware_set->bootable_firmware_sha256[1], 32);
    if (!entry->container_enabled) return ECONTAINER_SLOTS_OK;

    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const econtainer_slots_result_t result = econtainer_slots_reconcile(
        &s_product.provider.io, &s_product.provider.geometry,
        firmware_set, &state, &decision);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (state.sequence == 0U ||
        (state.phase != ECONTAINER_SLOT_IDLE && state.phase != ECONTAINER_SLOT_CONFIRMED) ||
        decision != ECONTAINER_SLOT_BOOT_CONFIRMED)
        return ECONTAINER_SLOTS_CONFLICT;
    if (entry->package_mode == ESP_BASE_OTA_NO_PACKAGE) {
        if (!state_has_firmware(&state, snapshot->source_sha256,
                                snapshot->inactive_sha256, false))
            return ECONTAINER_SLOTS_CONFLICT;
    } else {
        const econtainer_slot_binding_t *running = snapshot_running_binding(
            &state, snapshot->source_sha256, snapshot->inactive_sha256);
        if (running == NULL ||
            running->package_present != entry->source_package_expected ||
            (entry->package_mode == ESP_BASE_OTA_PACKAGE_REUSE &&
             !running->package_present)) return ECONTAINER_SLOTS_CONFLICT;
        if (running->package_present) {
            if (running->package_size_bytes == 0U ||
                running->guest_abi_version == 0U ||
                running->data_schema_version == 0U ||
                digest_zero(running->package_sha256))
                return ECONTAINER_SLOTS_CONFLICT;
            snapshot->source_package_present = true;
            snapshot->source_package_size_bytes = running->package_size_bytes;
            snapshot->source_guest_abi_version = running->guest_abi_version;
            snapshot->source_data_schema_version = running->data_schema_version;
            memcpy(snapshot->source_package_sha256, running->package_sha256, 32);
        }
    }
    /* Keep the existing NO_PACKAGE A-only invariant. Package modes rely on
     * Container reconcile and the exact running binding checked above. */
    if (entry->package_mode == ESP_BASE_OTA_NO_PACKAGE &&
        firmware_set->bootable_count == 1U && state.phase != ECONTAINER_SLOT_IDLE)
        return ECONTAINER_SLOTS_CONFLICT;
    const uint32_t retirement_steps =
        firmware_set->bootable_count == 2U ? 1U : 0U;
    const uint32_t recovery_commits = ESP_BASE_CONTAINER_OTA_RECOVERY_COMMITS +
        (entry->package_mode == ESP_BASE_OTA_PACKAGE_WRITE ? 1U : 0U);
    if (state.sequence > UINT32_MAX - retirement_steps -
                             recovery_commits)
        return ECONTAINER_SLOTS_CONFLICT;
    snapshot->container_enabled = true;
    snapshot->container_sequence = state.sequence;
    return ECONTAINER_SLOTS_OK;
}

bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim, esp_base_ota_package_mode_t package_mode,
    esp_base_ota_receipt_snapshot_t *snapshot)
{
    if (snapshot == NULL) return false;
    *snapshot = (esp_base_ota_receipt_snapshot_t){0};
    if (!esp_base_storage_claim_active(claim)) return false;
    const bool configured = policy_present();
    if (!snapshot_mode_ready(package_mode) ||
        (configured && !s_product.provider_bound))
        return false;
    const bool source_package_expected = configured &&
        atomic_load_explicit(&s_product.result, memory_order_acquire) ==
            ESP_BASE_CONTAINER_RUNNING;
    snapshot_context_t context = {.container_enabled = configured,
                                  .package_mode = package_mode,
                                  .source_package_expected = source_package_expected,
                                  .snapshot = snapshot};
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, snapshot_for_ota, &context);
    if (result == ECONTAINER_SLOTS_OK && snapshot_mode_ready(package_mode) &&
        (!configured || source_package_expected ==
            (atomic_load_explicit(&s_product.result, memory_order_acquire) ==
                ESP_BASE_CONTAINER_RUNNING))) return true;
    *snapshot = (esp_base_ota_receipt_snapshot_t){0};
    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_SNAPSHOT_BLOCKED result=%d", (int)result);
    return false;
}

static esp_base_container_retire_result_t retire_result(econtainer_slots_result_t result)
{
    if (result == ECONTAINER_SLOTS_OK) return ESP_BASE_CONTAINER_RETIRE_COMPLETE;
    if (result == ECONTAINER_SLOTS_CONFLICT || result == ECONTAINER_SLOTS_UNTRUSTED ||
        result == ECONTAINER_SLOTS_EMPTY || result == ECONTAINER_SLOTS_INVALID ||
        result == ECONTAINER_SLOTS_NO_SPACE) return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    return ESP_BASE_CONTAINER_RETIRE_UNCERTAIN;
}

typedef struct {
    uint32_t expected_sequence;
    const uint8_t *source_sha256;
    const uint8_t *inactive_sha256;
} retire_context_t;

static econtainer_slots_result_t verify_source_only(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const retire_context_t *retire = context;
    return firmware_set->bootable_count == 1U &&
           memcmp(firmware_set->running_firmware_sha256,
                  retire->source_sha256, 32) == 0 ?
           ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}

static econtainer_slots_result_t retire_inactive(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const retire_context_t *retire = context;
    if (firmware_set->bootable_count != 1U ||
        memcmp(firmware_set->running_firmware_sha256, retire->source_sha256, 32) != 0)
        return ECONTAINER_SLOTS_CONFLICT;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    const bool old_inactive = !digest_zero(retire->inactive_sha256);
    if (old_inactive && state.sequence == retire->expected_sequence &&
        (state.phase == ECONTAINER_SLOT_IDLE || state.phase == ECONTAINER_SLOT_CONFIRMED) &&
        state_has_firmware(&state, retire->source_sha256,
                            retire->inactive_sha256, false)) {
        result = econtainer_slots_retire_inactive_firmware(
            &s_product.provider.io, &s_product.provider.geometry,
            state.sequence, firmware_set, retire->inactive_sha256, &state);
        if (result != ECONTAINER_SLOTS_OK) return result;
    } else if (state.sequence != retire->expected_sequence + (old_inactive ? 1U : 0U) ||
               state.phase != ECONTAINER_SLOT_IDLE ||
               !state_has_firmware(&state, retire->source_sha256,
                                   (const uint8_t[32]){0}, false)) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    return verified_a_only(firmware_set);
}

esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32])
{
    if (!esp_base_storage_claim_active(claim) || source_sha256 == NULL ||
        inactive_sha256 == NULL || digest_zero(source_sha256) ||
        (!digest_zero(inactive_sha256) &&
         memcmp(source_sha256, inactive_sha256, 32) == 0) ||
        policy_present() != container_enabled ||
        (container_enabled ? expected_sequence == 0U : expected_sequence != 0U))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    if (container_enabled &&
        (!s_product.provider_bound || !esp_base_container_product_ota_ready()))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    retire_context_t context = {expected_sequence, source_sha256, inactive_sha256};
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        container_enabled ? retire_inactive :
            /* With no Container policy, still prove the physical A-only set. */
            verify_source_only, &context);
    return retire_result(result);
}

typedef struct {
    retire_context_t retire;
    const uint8_t *candidate_sha256;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
} recovery_context_t;

static bool exact_recovery_operation(const econtainer_slots_state_t *state,
                                     const recovery_context_t *recovery)
{
    return state->operation.firmware_transition &&
        state->operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
        memcmp(state->operation.operation_id, recovery->operation_id,
               sizeof recovery->operation_id) == 0 &&
        memcmp(state->operation.target_firmware_sha256,
               recovery->candidate_sha256, 32) == 0 &&
        state_has_firmware(state, recovery->retire.source_sha256,
                           recovery->candidate_sha256, true) &&
        /* A trial can be abandoned without a stop callback only on a
         * different boot, before product_boot creates its guest executor. */
        memcmp(state->operation.trial_boot_id, recovery->boot_id,
               sizeof recovery->boot_id) != 0;
}

static econtainer_slots_result_t recover_retired_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const recovery_context_t *recovery = context;
    const retire_context_t *retire = &recovery->retire;
    if (firmware_set->bootable_count != 1U ||
        memcmp(firmware_set->running_firmware_sha256, retire->source_sha256, 32) != 0)
        return ECONTAINER_SLOTS_CONFLICT;

    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    const bool old_inactive = !digest_zero(retire->inactive_sha256);
    const uint32_t retired_sequence = retire->expected_sequence +
                                      (old_inactive ? 1U : 0U);
    if (old_inactive && state.sequence == retire->expected_sequence &&
        (state.phase == ECONTAINER_SLOT_IDLE || state.phase == ECONTAINER_SLOT_CONFIRMED) &&
        state_has_firmware(&state, retire->source_sha256,
                            retire->inactive_sha256, false)) {
        return retire_inactive(firmware_set, (void *)retire);
    }
    if (state.phase == ECONTAINER_SLOT_IDLE &&
        state_has_firmware(&state, retire->source_sha256,
                            (const uint8_t[32]){0}, false)) {
        const uint32_t delta = state.sequence - retired_sequence;
        /* 0: retired before prepare; 3/4/5: stage, optional trial/health,
         * abandon, drop. No other sequence can be attributed to this receipt. */
        if (delta != 0U && delta != 3U && delta != 4U && delta != 5U)
            return ECONTAINER_SLOTS_CONFLICT;
        return verified_a_only(firmware_set);
    }
    if (!exact_recovery_operation(&state, recovery)) return ECONTAINER_SLOTS_CONFLICT;
    const uint32_t delta = state.sequence - retired_sequence;
    if ((state.phase == ECONTAINER_SLOT_PREPARED && delta != 1U) ||
        (state.phase == ECONTAINER_SLOT_TRIAL_STARTED && delta != 2U) ||
        (state.phase == ECONTAINER_SLOT_HEALTH_VERIFIED && delta != 3U) ||
        (state.phase == ECONTAINER_SLOT_ABORTED &&
         (delta < 2U || delta > 4U)) ||
        (state.phase != ECONTAINER_SLOT_PREPARED &&
         state.phase != ECONTAINER_SLOT_TRIAL_STARTED &&
         state.phase != ECONTAINER_SLOT_HEALTH_VERIFIED &&
         state.phase != ECONTAINER_SLOT_ABORTED)) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    if (state.phase != ECONTAINER_SLOT_ABORTED) {
        result = econtainer_slots_abandon(
            &s_product.provider.io, &s_product.provider.geometry,
            state.sequence, recovery->boot_id, NULL, NULL, &state);
        if (result != ECONTAINER_SLOTS_OK) return result;
        if (state.phase != ECONTAINER_SLOT_ABORTED ||
            !exact_recovery_operation(&state, recovery)) return ECONTAINER_SLOTS_UNCERTAIN;
    }
    result = econtainer_slots_drop_aborted_firmware(
        &s_product.provider.io, &s_product.provider.geometry,
        state.sequence, firmware_set, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (state.phase != ECONTAINER_SLOT_IDLE ||
        !state_has_firmware(&state, retire->source_sha256,
                            (const uint8_t[32]){0}, false))
        return ECONTAINER_SLOTS_UNCERTAIN;
    return verified_a_only(firmware_set);
}

esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32], const uint8_t candidate_sha256[32],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37])
{
    if (!esp_base_storage_claim_active(claim) || source_sha256 == NULL ||
        inactive_sha256 == NULL || candidate_sha256 == NULL ||
        operation_id == NULL || boot_id == NULL || digest_zero(source_sha256) ||
        digest_zero(candidate_sha256) ||
        (!digest_zero(inactive_sha256) &&
         memcmp(source_sha256, inactive_sha256, 32) == 0) ||
        policy_present() != container_enabled ||
        (container_enabled ? expected_sequence == 0U : expected_sequence != 0U) ||
        s_product.ready != NULL ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    recovery_context_t context = {
        .retire = {expected_sequence, source_sha256, inactive_sha256},
        .candidate_sha256 = candidate_sha256,
    };
    if (!decode_uuid(operation_id, context.operation_id) ||
        !decode_uuid(boot_id, context.boot_id))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    if (container_enabled && !ensure_provider(claim))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        container_enabled ? recover_retired_firmware : verify_source_only,
        container_enabled ? (void *)&context : (void *)&context.retire);
    return retire_result(result);
}

typedef struct {
    const esp_base_ota_receipt_recovery_t *receipt;
    eota_state_t running_state;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
} selected_ota_context_t;

static econtainer_slots_result_t verify_selected_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const selected_ota_context_t *selected = context;
    const esp_base_ota_receipt_recovery_t *receipt = selected->receipt;
    if (firmware_set->bootable_count != 2U ||
        memcmp(firmware_set->running_firmware_sha256,
               receipt->candidate_sha256, 32) != 0 ||
        memcmp(firmware_set->bootable_firmware_sha256[1],
               receipt->source_sha256, 32) != 0) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    return ECONTAINER_SLOTS_OK;
}

static econtainer_slots_result_t reconcile_selected_ota(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    if (verify_selected_firmware(firmware_set, context) != ECONTAINER_SLOTS_OK)
        return ECONTAINER_SLOTS_CONFLICT;
    const selected_ota_context_t *selected = context;
    const esp_base_ota_receipt_recovery_t *receipt = selected->receipt;
    const uint32_t retired_sequence = receipt->container_sequence +
        (digest_zero(receipt->inactive_sha256) ? 0U : 1U);
    const uint32_t confirmed_sequence = retired_sequence +
        ESP_BASE_CONTAINER_OTA_CONFIRM_COMMITS;
    if (receipt->status == ESP_BASE_OTA_RECEIPT_SUCCEEDED) {
        /* SUCCEEDED was persisted only after C was signed, VALID and the
         * receipt-bound ECS2 confirm completed. Later product-only operations
         * may replace that operation, but may not change the firmware set or
         * hide an incomplete firmware transition. Reconcile also rehashes all
         * confirmed package references and a complete pending candidate. */
        econtainer_slots_state_t current = {0};
        econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
        const econtainer_slots_result_t reconciled = econtainer_slots_reconcile(
            &s_product.provider.io, &s_product.provider.geometry,
            firmware_set, &current, &decision);
        if (reconciled != ECONTAINER_SLOTS_OK) return reconciled;
        if (decision != ECONTAINER_SLOT_BOOT_CONFIRMED &&
            decision != ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED)
            return ECONTAINER_SLOTS_CONFLICT;
        if (current.sequence < confirmed_sequence) return ECONTAINER_SLOTS_CONFLICT;
        if (current.sequence > confirmed_sequence)
            return !current.operation.firmware_transition &&
                current.phase != ECONTAINER_SLOT_IDLE &&
                memcmp(current.operation.target_firmware_sha256,
                       receipt->candidate_sha256, 32) == 0 ?
                ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
        return current.phase == ECONTAINER_SLOT_CONFIRMED &&
            current.operation.firmware_transition &&
            current.operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
            memcmp(current.operation.operation_id, selected->operation_id,
                   sizeof selected->operation_id) == 0 &&
            memcmp(current.operation.target_firmware_sha256,
                   receipt->candidate_sha256, 32) == 0 &&
            state_has_firmware(&current, receipt->source_sha256,
                               receipt->candidate_sha256, true) ?
            ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
    }
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    if (!state.operation.firmware_transition ||
        state.operation.kind != ECONTAINER_SLOT_NO_PACKAGE ||
        memcmp(state.operation.operation_id, selected->operation_id,
               sizeof selected->operation_id) != 0 ||
        memcmp(state.operation.target_firmware_sha256,
               receipt->candidate_sha256, 32) != 0 ||
        !state_has_firmware(&state, receipt->source_sha256,
                            receipt->candidate_sha256, true)) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    if (selected->running_state == EOTA_STATE_PENDING_VERIFY) {
        return receipt->status == ESP_BASE_OTA_RECEIPT_PREPARED &&
               state.phase == ECONTAINER_SLOT_PREPARED &&
               state.sequence == retired_sequence + 1U ?
               ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
    }
    if (selected->running_state != EOTA_STATE_VALID) return ECONTAINER_SLOTS_CONFLICT;
    if (state.phase == ECONTAINER_SLOT_CONFIRMED &&
        state.sequence == retired_sequence + 4U) return ECONTAINER_SLOTS_OK;
    if (receipt->status != ESP_BASE_OTA_RECEIPT_PREPARED ||
        state.phase != ECONTAINER_SLOT_HEALTH_VERIFIED ||
        state.sequence != retired_sequence + 3U)
        return ECONTAINER_SLOTS_CONFLICT;
    /* The original V2 receipt has already bound the exact operation, A/C
     * digests and sequence. Complete the durable confirm only in this
     * receipt-bound VALID boot; normal product startup never repairs it. */
    econtainer_slots_state_t confirmed = {0};
    const econtainer_slots_result_t result = econtainer_slots_confirm(
        &s_product.provider.io, &s_product.provider.geometry, state.sequence,
        firmware_set->running_firmware_sha256, state.operation.trial_boot_id,
        &confirmed);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (confirmed.phase != ECONTAINER_SLOT_CONFIRMED ||
        confirmed.sequence != retired_sequence + 4U ||
        memcmp(confirmed.operation.operation_id, selected->operation_id,
               sizeof selected->operation_id) != 0 ||
        !state_has_firmware(&confirmed, receipt->source_sha256,
                            receipt->candidate_sha256, true))
        return ECONTAINER_SLOTS_UNCERTAIN;
    ESP_LOGI(TAG, "ESP_BASE_CONTAINER_RECOVERED_VALID confirmed_sequence=%u",
             (unsigned)confirmed.sequence);
    return ECONTAINER_SLOTS_OK;
}

bool esp_base_container_product_reconcile_selected_ota(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt, eota_state_t running_state)
{
    if (!esp_base_storage_claim_active(claim) || receipt == NULL ||
        policy_present() != receipt->container_enabled ||
        (receipt->status != ESP_BASE_OTA_RECEIPT_PREPARED &&
         receipt->status != ESP_BASE_OTA_RECEIPT_SUCCEEDED) ||
        (running_state != EOTA_STATE_PENDING_VERIFY &&
         running_state != EOTA_STATE_VALID) ||
        (receipt->status == ESP_BASE_OTA_RECEIPT_SUCCEEDED &&
         running_state != EOTA_STATE_VALID)) return false;
    if (digest_zero(receipt->source_sha256) ||
        digest_zero(receipt->candidate_sha256) ||
        memcmp(receipt->source_sha256, receipt->candidate_sha256, 32) == 0)
        return false;
    selected_ota_context_t context = {.receipt = receipt,
                                      .running_state = running_state};
    const esp_base_ota_firmware_observation_t observation =
        running_state == EOTA_STATE_PENDING_VERIFY ?
            ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL :
            ESP_BASE_OTA_FIRMWARE_CONFIRMED;
    if (!receipt->container_enabled) {
        /* Reprove both signed images and IDF rollback after a reboot, even
         * when no product binding is configured. */
        return esp_base_container_with_firmware_set(
            claim, observation, NULL, verify_selected_firmware,
            &context) == ECONTAINER_SLOTS_OK;
    }
    const uint32_t retirement_steps =
        digest_zero(receipt->inactive_sha256) ? 0U : 1U;
    if (receipt->container_sequence == 0U ||
        receipt->container_sequence > UINT32_MAX - retirement_steps -
                                          ESP_BASE_CONTAINER_OTA_CONFIRM_COMMITS ||
        s_product.ready != NULL ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        !ensure_provider(claim)) return false;
    if (!decode_uuid(receipt->operation_id, context.operation_id)) return false;
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, observation,
        NULL, reconcile_selected_ota, &context);
    if (result != ECONTAINER_SLOTS_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_SELECTED_OTA_BLOCKED result=%d", (int)result);
        return false;
    }
    return true;
}

static esp_base_container_boot_result_t start_product(
    const esp_base_storage_claim_t *claim, bool trial_mode, bool package_trial_mode,
    uint32_t package_trial_sequence,
    const uint8_t package_trial_operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES],
    const char boot_id[37], const uint8_t trial_event_sha256[32])
{
    if (!esp_base_storage_claim_active(claim)) return ESP_BASE_CONTAINER_BLOCKED;
    if (!policy_present()) return ESP_BASE_CONTAINER_NOT_CONFIGURED;
    if (package_trial_mode && trial_event_sha256 == NULL)
        return ESP_BASE_CONTAINER_BLOCKED;
    if (s_product.thread_joinable ||
        (s_product.start_attempted && !s_product.reopen_allowed)) {
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    if (!decode_uuid(boot_id, s_product.boot_id)) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED invalid boot identity");
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    if (!ensure_provider(claim)) return ESP_BASE_CONTAINER_BLOCKED;
    s_product.trial_mode = trial_mode;
    s_product.package_trial_mode = package_trial_mode;
    s_product.package_trial_sequence = package_trial_sequence;
    if (package_trial_mode) {
        memcpy(s_product.package_trial_operation_id, package_trial_operation_id,
               sizeof s_product.package_trial_operation_id);
    } else {
        memset(s_product.package_trial_operation_id, 0,
               sizeof s_product.package_trial_operation_id);
    }
    atomic_store_explicit(&s_product.stop_requested, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.event_accepting, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.event_progress_count, 0, memory_order_relaxed);
    atomic_store_explicit(&s_product.instance_active, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.boot_admitted, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.result, ESP_BASE_CONTAINER_BLOCKED,
                          memory_order_relaxed);
    s_product.stop_succeeded = false;
    s_product.native_reclaimed = false;
    s_product.claim = claim;
    if (s_product.ready == NULL) s_product.ready = xSemaphoreCreateBinary();
    if (s_product.ready == NULL) return ESP_BASE_CONTAINER_BLOCKED;
    if (s_product.stopped == NULL) s_product.stopped = xSemaphoreCreateBinary();
    if (s_product.stopped == NULL) return ESP_BASE_CONTAINER_BLOCKED;
    if (s_product.event_lock == NULL) s_product.event_lock = xSemaphoreCreateMutex();
    if (s_product.event_lock == NULL) return ESP_BASE_CONTAINER_BLOCKED;
    if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE)
        return ESP_BASE_CONTAINER_BLOCKED;
    s_product.last_event_observation = (esp_base_container_event_observation_t){0};
    s_product.guest_call_processing = false;
    s_product.trial_commit_active = false;
    s_product.representative_event_sequence = 0U;
    s_product.trial_failure_count = 0U;
    if (package_trial_mode)
        memcpy(s_product.trial_event_sha256, trial_event_sha256, 32);
    else
        memset(s_product.trial_event_sha256, 0, 32);
    xSemaphoreGive(s_product.event_lock);
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return ESP_BASE_CONTAINER_BLOCKED;
    const bool valid_thread =
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_JOINABLE) == 0 &&
        pthread_attr_setstacksize(&attributes,
            CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES) == 0;
    s_product.start_attempted = true;
    s_product.reopen_allowed = false;
    const int created = valid_thread ?
        pthread_create(&s_product.thread, &attributes, product_thread, NULL) : -1;
    (void)pthread_attr_destroy(&attributes);
    if (created != 0) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED executor create=%d", created);
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    s_product.thread_joinable = true;
    if (xSemaphoreTake(s_product.ready, portMAX_DELAY) != pdTRUE) {
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    /* The worker used the borrowed claim only before publishing readiness. */
    s_product.claim = NULL;
    const esp_base_container_boot_result_t result =
        (esp_base_container_boot_result_t)atomic_load_explicit(
        &s_product.result, memory_order_acquire);
    if (result != ESP_BASE_CONTAINER_RUNNING) {
        if (xSemaphoreTake(s_product.stopped, portMAX_DELAY) != pdTRUE ||
            pthread_join(s_product.thread, NULL) != 0) {
            return ESP_BASE_CONTAINER_BLOCKED;
        }
        s_product.thread_joinable = false;
        /* EMPTY has no executor to reclaim. A later caller still needs an
         * active Base owner, and the next worker rechecks the real slot. */
        s_product.reopen_allowed = !trial_mode && result == ESP_BASE_CONTAINER_EMPTY &&
            s_product.stop_succeeded &&
            !atomic_load_explicit(&s_product.instance_active, memory_order_acquire);
    }
    return result;
}

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    return start_product(claim, false, false, 0U, NULL, boot_id, NULL);
}

static econtainer_slots_result_t pristine_baseline(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)context;
    return esp_base_container_pristine_no_package(
        &s_product.provider.io, &s_product.provider.geometry, firmware_set);
}

bool esp_base_container_product_pristine_baseline(
    const esp_base_storage_claim_t *claim)
{
    if (!policy_present() || !esp_base_storage_claim_active(claim) ||
        !s_product.provider_bound || !s_product.start_attempted ||
        s_product.thread_joinable || s_product.trial_mode ||
        !atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_EMPTY || s_product.uninstall_uncertain) {
        return false;
    }
    return esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        pristine_baseline, NULL) == ECONTAINER_SLOTS_OK;
}

esp_base_container_boot_result_t esp_base_container_product_start_trial(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    return start_product(claim, true, false, 0U, NULL, boot_id, NULL);
}

typedef struct {
    uint32_t sequence;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
} package_trial_preflight_t;

static econtainer_slots_result_t preflight_package_trial(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const package_trial_preflight_t *trial = context;
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    return state.sequence == trial->sequence &&
        state.phase == ECONTAINER_SLOT_PREPARED &&
        state.operation.kind == ECONTAINER_SLOT_PACKAGE_WRITE &&
        !state.operation.firmware_transition &&
        memcmp(state.operation.operation_id, trial->operation_id,
               sizeof state.operation.operation_id) == 0 &&
        memcmp(state.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) == 0 ?
        ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}

esp_base_container_boot_result_t esp_base_container_product_start_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37], const uint8_t trial_event_sha256[32])
{
    uint8_t decoded_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {0};
    if (!esp_base_storage_claim_active(claim) || prepared_sequence == 0U ||
        prepared_sequence >= UINT32_MAX - 1U ||
        !decode_uuid(operation_id, decoded_id) || trial_event_sha256 == NULL ||
        !s_product.reopen_allowed ||
        s_product.thread_joinable || !s_product.native_reclaimed ||
        s_product.trial_mode ||
        (atomic_load_explicit(&s_product.result, memory_order_acquire) !=
             ESP_BASE_CONTAINER_STOPPED &&
         atomic_load_explicit(&s_product.result, memory_order_acquire) !=
             ESP_BASE_CONTAINER_EMPTY)) {
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    package_trial_preflight_t preflight = {.sequence = prepared_sequence};
    memcpy(preflight.operation_id, decoded_id, sizeof decoded_id);
    if (esp_base_container_with_firmware_set(
            claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
            preflight_package_trial, &preflight) != ECONTAINER_SLOTS_OK) {
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    return start_product(claim, true, true, prepared_sequence, decoded_id, boot_id,
                         trial_event_sha256);
}

static econtainer_slots_result_t mark_trial_healthy(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)firmware_set;
    (void)context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (!state.operation.firmware_transition) return ECONTAINER_SLOTS_CONFLICT;
    return econtainer_slots_mark_healthy(
        &s_product.provider.io, &s_product.provider.geometry, state.sequence,
        s_product.boot_id, &state);
}

bool esp_base_container_product_mark_healthy(const esp_base_storage_claim_t *claim)
{
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    if (!s_product.trial_mode || !esp_base_storage_claim_active(claim) ||
        result != ESP_BASE_CONTAINER_EMPTY) {
        return false;
    }
    const econtainer_slots_result_t marked = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL, NULL, mark_trial_healthy, NULL);
    if (marked != ECONTAINER_SLOTS_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_HEALTH_BLOCKED result=%d", (int)marked);
        return false;
    }
    return true;
}

typedef struct {
    const uint8_t *boot_id;
} confirm_context_t;

static econtainer_slots_result_t confirm_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const confirm_context_t *confirm = context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (!state.operation.firmware_transition) return ECONTAINER_SLOTS_CONFLICT;
    return econtainer_slots_confirm(
        &s_product.provider.io, &s_product.provider.geometry, state.sequence,
        firmware_set->running_firmware_sha256, confirm->boot_id, &state);
}

bool esp_base_container_product_confirm_firmware(const esp_base_storage_claim_t *claim)
{
    if (!s_product.trial_mode || s_product.package_trial_mode ||
        !esp_base_storage_claim_active(claim)) return false;
    const confirm_context_t confirm = {.boot_id = s_product.boot_id};
    /* This observation independently requires signed C and otadata VALID,
     * after eota_confirm_pending returned and Base inspected the selector. */
    const econtainer_slots_result_t committed = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, confirm_firmware,
        (void *)&confirm);
    if (committed != ECONTAINER_SLOTS_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_CONFIRM_BLOCKED result=%d", (int)committed);
        return false;
    }
    return true;
}

bool esp_base_container_product_stop_trial(const esp_base_storage_claim_t *claim)
{
    if (!s_product.trial_mode) return true;
    if (!esp_base_storage_claim_active(claim) || s_product.stopped == NULL) return false;
    if (!s_product.thread_joinable) {
        return s_product.stop_succeeded &&
            !atomic_load_explicit(&s_product.instance_active, memory_order_acquire);
    }
    atomic_store_explicit(&s_product.stop_requested, true, memory_order_release);
    if (xSemaphoreTake(s_product.stopped, pdMS_TO_TICKS(5000U)) != pdTRUE) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STOP_UNPROVEN executor timeout");
        return false;
    }
    if (pthread_join(s_product.thread, NULL) != 0) return false;
    s_product.thread_joinable = false;
    s_product.claim = NULL;
    if (!s_product.stop_succeeded ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire)) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STOP_UNPROVEN native cleanup");
        return false;
    }
    return true;
}

static bool package_trial_stopped(
    void *context, const uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES])
{
    (void)context;
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    return s_product.package_trial_mode && !s_product.thread_joinable &&
        s_product.native_reclaimed && !
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire) &&
        (result == ESP_BASE_CONTAINER_STOPPED || result == ESP_BASE_CONTAINER_BLOCKED) &&
        memcmp(operation_id, s_product.package_trial_operation_id,
               ECONTAINER_SLOT_OPERATION_ID_BYTES) == 0;
}

typedef struct {
    uint32_t trial_sequence;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
} package_abandon_context_t;

static bool same_binding(const econtainer_slot_binding_t *left,
                         const econtainer_slot_binding_t *right);
static int binding_index(const econtainer_slots_state_t *state,
                         const uint8_t firmware_sha256[32]);
static bool bindings_match_firmware_set(
    const econtainer_slots_state_t *state,
    const econtainer_slot_firmware_set_t *firmware_set);

static econtainer_slots_result_t abandon_package_trial(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const package_abandon_context_t *abandon = context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (state.sequence != abandon->trial_sequence ||
        state.phase != ECONTAINER_SLOT_TRIAL_STARTED ||
        state.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        state.operation.firmware_transition ||
        memcmp(state.operation.operation_id, abandon->operation_id,
               sizeof state.operation.operation_id) != 0 ||
        memcmp(state.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0 ||
        memcmp(state.operation.trial_boot_id, s_product.boot_id,
               sizeof s_product.boot_id) != 0) return ECONTAINER_SLOTS_CONFLICT;
    result = econtainer_slots_abandon(
        &s_product.provider.io, &s_product.provider.geometry, state.sequence,
        s_product.boot_id, package_trial_stopped, NULL, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    econtainer_slots_state_t readback = {0};
    result = econtainer_slots_load(&s_product.provider.io,
                                   &s_product.provider.geometry, &readback);
    if (result != ECONTAINER_SLOTS_OK ||
        readback.sequence != abandon->trial_sequence + 1U ||
        readback.phase != ECONTAINER_SLOT_ABORTED ||
        readback.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        readback.operation.firmware_transition ||
        memcmp(readback.operation.operation_id, abandon->operation_id,
               sizeof readback.operation.operation_id) != 0 ||
        memcmp(readback.operation.package_sha256,
               state.operation.package_sha256, 32) != 0) {
        return ECONTAINER_SLOTS_UNCERTAIN;
    }
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (!same_binding(&readback.bindings[index], &state.bindings[index]))
            return ECONTAINER_SLOTS_UNCERTAIN;
    }
    return ECONTAINER_SLOTS_OK;
}

bool esp_base_container_product_abandon_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES])
{
    package_abandon_context_t context = {.trial_sequence = trial_sequence};
    if (!esp_base_storage_claim_active(claim) || !s_product.package_trial_mode ||
        trial_sequence == 0U || trial_sequence == UINT32_MAX ||
        !decode_uuid(operation_id, context.operation_id) ||
        memcmp(context.operation_id, s_product.package_trial_operation_id,
               sizeof context.operation_id) != 0) return false;
    const bool stopped = esp_base_container_product_stop_trial(claim);
    if (!stopped && (s_product.thread_joinable || !s_product.native_reclaimed ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_BLOCKED)) return false;
    /* A trap or timer failure can finish the guest before stop_requested is
     * seen. A joined, reclaimed BLOCKED trial is still safe to abandon. */
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, abandon_package_trial,
        &context);
    if (result != ECONTAINER_SLOTS_OK) return false;
    s_product.reopen_allowed = true;
    return true;
}

typedef struct {
    uint32_t trial_sequence;
    uint32_t confirmed_sequence;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t observed_package_sha256[32];
} package_confirm_context_t;

static econtainer_slots_result_t confirm_package_trial(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    package_confirm_context_t *confirm = context;
    econtainer_slots_state_t trial = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &trial);
    if (result != ECONTAINER_SLOTS_OK) return result;
    const int index = binding_index(&trial, firmware_set->running_firmware_sha256);
    if (index < 0 || !bindings_match_firmware_set(&trial, firmware_set) ||
        trial.sequence != confirm->trial_sequence ||
        trial.phase != ECONTAINER_SLOT_TRIAL_STARTED ||
        trial.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        trial.operation.firmware_transition ||
        memcmp(trial.operation.operation_id, confirm->operation_id,
               sizeof confirm->operation_id) != 0 ||
        memcmp(trial.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0 ||
        memcmp(trial.operation.package_sha256,
               confirm->observed_package_sha256, 32) != 0 ||
        memcmp(trial.operation.trial_boot_id, s_product.boot_id,
               sizeof s_product.boot_id) != 0) return ECONTAINER_SLOTS_CONFLICT;
    if (!atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_RUNNING) return ECONTAINER_SLOTS_UNCERTAIN;
    const econtainer_slot_binding_t previous = trial.bindings[index];
    const econtainer_slot_binding_t other = trial.bindings[1 - index];

    econtainer_slots_state_t healthy = {0};
    result = econtainer_slots_mark_healthy(
        &s_product.provider.io, &s_product.provider.geometry, trial.sequence,
        s_product.boot_id, &healthy);
    if (result != ECONTAINER_SLOTS_OK) return result;
    econtainer_slots_state_t readback = {0};
    result = econtainer_slots_load(&s_product.provider.io,
                                   &s_product.provider.geometry, &readback);
    if (result != ECONTAINER_SLOTS_OK ||
        readback.sequence != trial.sequence + 1U ||
        readback.phase != ECONTAINER_SLOT_HEALTH_VERIFIED ||
        memcmp(readback.operation.operation_id, confirm->operation_id,
               sizeof confirm->operation_id) != 0 ||
        memcmp(readback.operation.package_sha256,
               trial.operation.package_sha256, 32) != 0 ||
        !same_binding(&readback.bindings[index], &previous) ||
        !same_binding(&readback.bindings[1 - index], &other))
        return ECONTAINER_SLOTS_UNCERTAIN;
    if (!atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_RUNNING) return ECONTAINER_SLOTS_UNCERTAIN;

    econtainer_slots_state_t committed = {0};
    result = econtainer_slots_confirm(&s_product.provider.io,
        &s_product.provider.geometry, readback.sequence,
        firmware_set->running_firmware_sha256, s_product.boot_id, &committed);
    if (result != ECONTAINER_SLOTS_OK) return result;
    result = econtainer_slots_load(&s_product.provider.io,
                                   &s_product.provider.geometry, &readback);
    if (result != ECONTAINER_SLOTS_OK ||
        readback.sequence != trial.sequence + 2U ||
        readback.phase != ECONTAINER_SLOT_CONFIRMED ||
        memcmp(readback.operation.operation_id, confirm->operation_id,
               sizeof confirm->operation_id) != 0 ||
        !same_binding(&readback.bindings[1 - index], &other) ||
        !readback.bindings[index].package_present ||
        readback.bindings[index].slot != trial.operation.slot ||
        readback.bindings[index].package_size_bytes !=
            trial.operation.package_size_bytes ||
        readback.bindings[index].guest_abi_version !=
            trial.operation.guest_abi_version ||
        readback.bindings[index].data_schema_version !=
            trial.operation.data_schema_version ||
        memcmp(readback.bindings[index].package_sha256,
               trial.operation.package_sha256, 32) != 0)
        return ECONTAINER_SLOTS_UNCERTAIN;
    confirm->confirmed_sequence = readback.sequence;
    return ECONTAINER_SLOTS_OK;
}

bool esp_base_container_product_trial_quiescent(void)
{
    if (s_product.event_lock == NULL ||
        xSemaphoreTake(s_product.event_lock, 0U) != pdTRUE) return false;
    const bool quiescent = s_product.trial_mode &&
        s_product.package_trial_mode &&
        s_product.event_count == 0U && !s_product.guest_call_processing &&
        !s_product.trial_commit_active &&
        esp_base_container_product_event_accepting();
    xSemaphoreGive(s_product.event_lock);
    return quiescent;
}

esp_base_container_trial_confirm_result_t esp_base_container_product_confirm_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint64_t verified_event_sequence, const uint8_t verified_event_sha256[32],
    uint64_t verified_failure_count,
    uint32_t *confirmed_sequence)
{
    if (confirmed_sequence != NULL) *confirmed_sequence = 0U;
    package_confirm_context_t confirm = {.trial_sequence = trial_sequence};
    if (!esp_base_storage_claim_active(claim) || confirmed_sequence == NULL ||
        verified_event_sequence == 0U || verified_event_sha256 == NULL ||
        trial_sequence == 0U ||
        trial_sequence > UINT32_MAX - 2U ||
        !decode_uuid(operation_id, confirm.operation_id) ||
        !s_product.trial_mode || !s_product.package_trial_mode ||
        s_product.package_trial_sequence != trial_sequence - 1U ||
        memcmp(confirm.operation_id, s_product.package_trial_operation_id,
               sizeof confirm.operation_id) != 0 ||
        !s_product.thread_joinable ||
        !atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_RUNNING) return ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    if (s_product.event_lock == NULL ||
        xSemaphoreTake(s_product.event_lock, 0U) != pdTRUE)
        return ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    const bool settled =
        s_product.representative_event_sequence == verified_event_sequence &&
        memcmp(s_product.trial_event_sha256, verified_event_sha256, 32) == 0 &&
        s_product.trial_failure_count == verified_failure_count &&
        verified_failure_count != UINT64_MAX &&
        s_product.event_count == 0U && !s_product.guest_call_processing &&
        !s_product.trial_commit_active &&
        esp_base_container_product_event_accepting();
    if (settled) {
        /* offer_event checks the same flag again under this lock. No new
         * business frame or timer call can enter during the commit. */
        memcpy(confirm.observed_package_sha256, s_product.event_package_sha256,
               sizeof confirm.observed_package_sha256);
        s_product.trial_commit_active = true;
        atomic_store_explicit(&s_product.event_accepting, false,
                              memory_order_release);
    }
    xSemaphoreGive(s_product.event_lock);
    if (!settled) return ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        confirm_package_trial, &confirm);
    if (result != ECONTAINER_SLOTS_OK ||
        !atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_RUNNING) return ESP_BASE_CONTAINER_CONFIRM_UNCERTAIN;
    if (xSemaphoreTake(s_product.event_lock, portMAX_DELAY) != pdTRUE)
        return ESP_BASE_CONTAINER_CONFIRM_UNCERTAIN;
    *confirmed_sequence = confirm.confirmed_sequence;
    s_product.trial_mode = false;
    s_product.package_trial_mode = false;
    s_product.package_trial_sequence = 0U;
    memset(s_product.package_trial_operation_id, 0,
           sizeof s_product.package_trial_operation_id);
    memset(s_product.trial_event_sha256, 0, sizeof s_product.trial_event_sha256);
    s_product.representative_event_sequence = 0U;
    s_product.trial_failure_count = 0U;
    s_product.trial_commit_active = false;
    atomic_store_explicit(&s_product.event_accepting, true,
                          memory_order_release);
    xSemaphoreGive(s_product.event_lock);
    return ESP_BASE_CONTAINER_CONFIRM_CONFIRMED;
}

bool esp_base_container_product_stop_confirmed(
    const esp_base_storage_claim_t *claim)
{
    if (!esp_base_storage_claim_active(claim) || s_product.trial_mode ||
        !s_product.thread_joinable ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_RUNNING) {
        return false;
    }
    atomic_store_explicit(&s_product.stop_requested, true, memory_order_release);
    if (xSemaphoreTake(s_product.stopped, pdMS_TO_TICKS(5000U)) != pdTRUE ||
        pthread_join(s_product.thread, NULL) != 0) {
        return false;
    }
    s_product.thread_joinable = false;
    s_product.claim = NULL;
    if (!s_product.stop_succeeded ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire) ||
        atomic_load_explicit(&s_product.result, memory_order_acquire) !=
            ESP_BASE_CONTAINER_STOPPED) {
        return false;
    }
    s_product.reopen_allowed = !s_product.uninstall_uncertain;
    return true;
}

typedef struct {
    const esp_base_storage_claim_t *claim;
    uint32_t expected_sequence;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t package_sha256[32];
    uint8_t running_firmware_sha256[32];
} uninstall_context_t;

static int binding_index(const econtainer_slots_state_t *state,
                         const uint8_t firmware_sha256[32])
{
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (state->bindings[index].present &&
            memcmp(state->bindings[index].firmware_sha256, firmware_sha256, 32) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool bindings_match_firmware_set(
    const econtainer_slots_state_t *state,
    const econtainer_slot_firmware_set_t *firmware_set)
{
    /* A valid blob may still refer to a different physical firmware set.
     * Every present binding must match one actual bootable digest exactly. */
    if (firmware_set->bootable_count == 0U ||
        firmware_set->bootable_count > ECONTAINER_SLOT_BINDING_COUNT) return false;
    unsigned matched = 0U;
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        if (!binding->present) continue;
        bool found = false;
        for (unsigned firmware = 0; firmware < firmware_set->bootable_count; ++firmware) {
            if (memcmp(binding->firmware_sha256,
                       firmware_set->bootable_firmware_sha256[firmware], 32) == 0) {
                if (found ||
                    (matched & (1U << firmware)) != 0U) return false;
                matched |= 1U << firmware;
                found = true;
            }
        }
        if (!found) return false;
    }
    return matched == (1U << firmware_set->bootable_count) - 1U;
}

static econtainer_slots_result_t read_binding_snapshot(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    const bool live_package_trial =
        state.phase == ECONTAINER_SLOT_TRIAL_STARTED &&
        s_product.package_trial_mode && s_product.trial_mode &&
        s_product.package_trial_sequence != UINT32_MAX &&
        state.sequence == s_product.package_trial_sequence + 1U &&
        state.operation.kind == ECONTAINER_SLOT_PACKAGE_WRITE &&
        !state.operation.firmware_transition &&
        memcmp(state.operation.operation_id, s_product.package_trial_operation_id,
               sizeof state.operation.operation_id) == 0 &&
        memcmp(state.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) == 0 &&
        memcmp(state.operation.trial_boot_id, s_product.boot_id,
               sizeof state.operation.trial_boot_id) == 0;
    if (state.sequence == 0U ||
        (state.phase != ECONTAINER_SLOT_IDLE &&
         state.phase != ECONTAINER_SLOT_CONFIRMED &&
         state.phase != ECONTAINER_SLOT_ABORTED &&
         !live_package_trial) ||
        (state.operation.firmware_transition &&
         (state.phase == ECONTAINER_SLOT_ABORTED ||
          (state.phase == ECONTAINER_SLOT_CONFIRMED &&
           memcmp(state.operation.target_firmware_sha256,
                  firmware_set->running_firmware_sha256, 32) != 0))) ||
        !bindings_match_firmware_set(&state, firmware_set)) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    const int index = binding_index(&state, firmware_set->running_firmware_sha256);
    if (index < 0) return ECONTAINER_SLOTS_CONFLICT;
    const econtainer_slot_binding_t *running = &state.bindings[index];
    esp_base_container_binding_snapshot_t *out = context;
    out->container_sequence = state.sequence;
    out->package_present = running->package_present;
    if (running->package_present)
        memcpy(out->package_sha256, running->package_sha256, sizeof out->package_sha256);
    return ECONTAINER_SLOTS_OK;
}

esp_base_container_binding_result_t esp_base_container_product_binding_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *out)
{
    if (out == NULL) return ESP_BASE_CONTAINER_BINDING_UNCERTAIN;
    *out = (esp_base_container_binding_snapshot_t){0};
    if (!policy_present()) return ESP_BASE_CONTAINER_BINDING_NOT_CONFIGURED;
    if (!esp_base_storage_claim_active(claim)) return ESP_BASE_CONTAINER_BINDING_BUSY;
    if (!s_product.provider_bound || s_product.uninstall_uncertain)
        return ESP_BASE_CONTAINER_BINDING_UNCERTAIN;
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, read_binding_snapshot, out);
    if (result == ECONTAINER_SLOTS_OK) return ESP_BASE_CONTAINER_BINDING_OK;
    *out = (esp_base_container_binding_snapshot_t){0};
    return result == ECONTAINER_SLOTS_BUSY ? ESP_BASE_CONTAINER_BINDING_BUSY :
           ESP_BASE_CONTAINER_BINDING_UNCERTAIN;
}

typedef struct {
    econtainer_package_workspace_t package;
    econtainer_wasm_workspace_t wasm;
    econtainer_package_info_t info;
} package_prepare_workspace_t;

typedef struct {
    const esp_base_container_package_request_t *request;
    econtainer_slot_source_fn source_fn;
    void *source_context;
    econtainer_package_slot_validation_t validation;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    esp_base_container_prepare_result_t outcome;
    uint32_t prepared_sequence;
    uint32_t aborted_sequence;
} package_prepare_context_t;

static bool package_prepare_guest_ready(bool previous_package_present)
{
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    return atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire) &&
        (previous_package_present ?
            (result == ESP_BASE_CONTAINER_RUNNING &&
             atomic_load_explicit(&s_product.instance_active, memory_order_acquire)) :
            (result == ESP_BASE_CONTAINER_EMPTY && !s_product.thread_joinable));
}

static bool package_prepare_binding_matches(
    const econtainer_slots_state_t *state,
    const econtainer_slot_firmware_set_t *firmware_set,
    const esp_base_container_package_request_t *request)
{
    const int index = binding_index(state, firmware_set->running_firmware_sha256);
    if (index < 0 || state->sequence != request->expected_sequence ||
        (state->phase != ECONTAINER_SLOT_IDLE &&
         state->phase != ECONTAINER_SLOT_CONFIRMED &&
         state->phase != ECONTAINER_SLOT_ABORTED) ||
        !bindings_match_firmware_set(state, firmware_set)) return false;
    const econtainer_slot_binding_t *binding = &state->bindings[index];
    return binding->package_present == request->previous_package_present &&
        (!binding->package_present ||
         memcmp(binding->package_sha256, request->previous_package_sha256, 32) == 0);
}

static bool package_prepare_operation_matches(
    const econtainer_slots_state_t *state,
    const package_prepare_context_t *prepare)
{
    const esp_base_container_package_request_t *request = prepare->request;
    return memcmp(state->operation.operation_id, prepare->operation_id, 16) == 0 &&
        memcmp(state->operation.package_sha256, request->package_sha256, 32) == 0 &&
        state->operation.package_size_bytes == request->package_size_bytes &&
        state->operation.guest_abi_version == request->guest_abi_version &&
        state->operation.data_schema_version == request->data_schema_version &&
        state->operation.kind == ECONTAINER_SLOT_PACKAGE_WRITE &&
        !state->operation.firmware_transition;
}

static bool package_prepare_abandon(
    package_prepare_context_t *prepare, uint32_t reserved_sequence,
    const econtainer_slot_firmware_set_t *firmware_set)
{
    econtainer_slots_state_t current = {0};
    if (econtainer_slots_load(&s_product.provider.io,
            &s_product.provider.geometry, &current) != ECONTAINER_SLOTS_OK ||
        current.sequence < reserved_sequence ||
        (current.phase != ECONTAINER_SLOT_WRITING &&
         current.phase != ECONTAINER_SLOT_PREPARED) ||
        !bindings_match_firmware_set(&current, firmware_set) ||
        !package_prepare_operation_matches(&current, prepare)) return false;
    const uint32_t sequence = current.sequence;
    econtainer_slots_state_t abandoned = {0};
    if (econtainer_slots_abandon(&s_product.provider.io,
            &s_product.provider.geometry, sequence, s_product.boot_id,
            NULL, NULL, &abandoned) != ECONTAINER_SLOTS_OK) return false;
    econtainer_slots_state_t readback = {0};
    if (econtainer_slots_load(&s_product.provider.io,
               &s_product.provider.geometry, &readback) == ECONTAINER_SLOTS_OK &&
        readback.sequence == sequence + 1U &&
        readback.phase == ECONTAINER_SLOT_ABORTED &&
        package_prepare_operation_matches(&readback, prepare) &&
        bindings_match_firmware_set(&readback, firmware_set)) {
        for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
            if (!same_binding(&current.bindings[index], &readback.bindings[index]))
                return false;
        }
        prepare->aborted_sequence = readback.sequence;
        return true;
    }
    return false;
}

static econtainer_slots_result_t package_prepare_with_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    package_prepare_context_t *prepare = context;
    const esp_base_container_package_request_t *request = prepare->request;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (!package_prepare_binding_matches(&state, firmware_set, request) ||
        !package_prepare_guest_ready(request->previous_package_present)) {
        prepare->outcome = ESP_BASE_CONTAINER_PREPARE_REJECTED;
        return ECONTAINER_SLOTS_CONFLICT;
    }

    econtainer_slot_operation_t operation = {.kind = ECONTAINER_SLOT_PACKAGE_WRITE,
        .package_size_bytes = request->package_size_bytes,
        .guest_abi_version = request->guest_abi_version,
        .data_schema_version = request->data_schema_version};
    memcpy(operation.operation_id, prepare->operation_id, sizeof operation.operation_id);
    memcpy(operation.target_firmware_sha256,
           firmware_set->running_firmware_sha256, 32);
    memcpy(operation.package_sha256, request->package_sha256, 32);
    result = econtainer_slots_reserve(&s_product.provider.io,
        &s_product.provider.geometry, request->expected_sequence,
        firmware_set, &operation, &state);
    if (result != ECONTAINER_SLOTS_OK) {
        prepare->outcome = result == ECONTAINER_SLOTS_BUSY ?
            ESP_BASE_CONTAINER_PREPARE_BUSY :
            result == ECONTAINER_SLOTS_CONFLICT || result == ECONTAINER_SLOTS_NO_SPACE ||
            result == ECONTAINER_SLOTS_INVALID ?
            ESP_BASE_CONTAINER_PREPARE_REJECTED :
            ESP_BASE_CONTAINER_PREPARE_UNCERTAIN;
        return result;
    }
    const uint32_t reserved_sequence = state.sequence;
    result = econtainer_slots_write_and_prepare(&s_product.provider.io,
        &s_product.provider.geometry, reserved_sequence,
        prepare->source_fn, prepare->source_context,
        econtainer_package_slot_validate, &prepare->validation, &state);
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t readback = {0};
        result = econtainer_slots_load(&s_product.provider.io,
            &s_product.provider.geometry, &readback);
        if (result == ECONTAINER_SLOTS_OK &&
            (readback.sequence != reserved_sequence + 1U ||
             readback.phase != ECONTAINER_SLOT_PREPARED ||
             !package_prepare_operation_matches(&readback, prepare))) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
        if (result == ECONTAINER_SLOTS_OK &&
            package_prepare_guest_ready(request->previous_package_present)) {
            prepare->prepared_sequence = readback.sequence;
            prepare->outcome = ESP_BASE_CONTAINER_PREPARED;
            return ECONTAINER_SLOTS_OK;
        }
    }
    /* A prepared candidate is still inactive. Its old binding remains intact,
     * so an exact WRITING/PREPARED record can be abandoned without stopping it. */
    if (!package_prepare_abandon(prepare, reserved_sequence, firmware_set)) {
        prepare->outcome = ESP_BASE_CONTAINER_PREPARE_UNCERTAIN;
        return ECONTAINER_SLOTS_UNCERTAIN;
    }
    prepare->outcome = ESP_BASE_CONTAINER_PREPARE_REJECTED;
    return ECONTAINER_SLOTS_CONFLICT;
}

esp_base_container_prepare_result_t esp_base_container_product_prepare_package(
    const esp_base_storage_claim_t *claim,
    const esp_base_container_package_request_t *request,
    econtainer_slot_source_fn source_fn, void *source_context,
    uint32_t *prepared_sequence)
{
    if (prepared_sequence != NULL) *prepared_sequence = 0U;
    if (!esp_base_storage_claim_active(claim)) return ESP_BASE_CONTAINER_PREPARE_BUSY;
    if (!policy_present() || request == NULL || source_fn == NULL ||
        prepared_sequence == NULL || !s_product.provider_bound ||
        !s_product.start_attempted || s_product.trial_mode ||
        s_product.uninstall_uncertain || request->expected_sequence == 0U ||
        request->expected_sequence > UINT32_MAX - 5U ||
        request->package_size_bytes == 0U ||
        request->guest_abi_version == 0U ||
        request->data_schema_version == 0U) {
        return ESP_BASE_CONTAINER_PREPARE_REJECTED;
    }
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {0};
    if (!decode_uuid(request->operation_id, operation_id))
        return ESP_BASE_CONTAINER_PREPARE_REJECTED;
    unsigned digest_or = 0U, previous_or = 0U;
    for (size_t index = 0; index < 32U; ++index) {
        digest_or |= request->package_sha256[index];
        previous_or |= request->previous_package_sha256[index];
    }
    if (digest_or == 0U ||
        (request->previous_package_present != (previous_or != 0U)) ||
        !package_prepare_guest_ready(request->previous_package_present)) {
        return ESP_BASE_CONTAINER_PREPARE_REJECTED;
    }
    package_prepare_workspace_t *workspace = calloc(1, sizeof *workspace);
    if (workspace == NULL) return ESP_BASE_CONTAINER_PREPARE_BUSY;
    package_prepare_context_t prepare = {
        .request = request, .source_fn = source_fn,
        .source_context = source_context,
        .validation = s_product.validation,
        .outcome = ESP_BASE_CONTAINER_PREPARE_UNCERTAIN,
    };
    memcpy(prepare.operation_id, operation_id, sizeof operation_id);
    prepare.validation.package_workspace = &workspace->package;
    prepare.validation.wasm_workspace = &workspace->wasm;
    prepare.validation.verified_info = &workspace->info;
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        package_prepare_with_firmware, &prepare);
    free(workspace);
    if (result == ECONTAINER_SLOTS_UNCERTAIN) return ESP_BASE_CONTAINER_PREPARE_UNCERTAIN;
    if (result == ECONTAINER_SLOTS_BUSY) return ESP_BASE_CONTAINER_PREPARE_BUSY;
    if (result == ECONTAINER_SLOTS_OK &&
        prepare.outcome == ESP_BASE_CONTAINER_PREPARED) {
        *prepared_sequence = prepare.prepared_sequence;
        return ESP_BASE_CONTAINER_PREPARED;
    }
    if (prepare.outcome == ESP_BASE_CONTAINER_PREPARE_REJECTED)
        *prepared_sequence = prepare.aborted_sequence;
    return prepare.outcome;
}

typedef struct {
    uint32_t prepared_sequence;
    uint32_t aborted_sequence;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t package_sha256[32];
} prepared_abandon_context_t;

static econtainer_slots_result_t abandon_prepared_package(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    prepared_abandon_context_t *abandon = context;
    econtainer_slots_state_t before = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &before);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (before.sequence != abandon->prepared_sequence ||
        before.phase != ECONTAINER_SLOT_PREPARED ||
        !bindings_match_firmware_set(&before, firmware_set) ||
        before.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        before.operation.firmware_transition ||
        memcmp(before.operation.operation_id, abandon->operation_id,
               sizeof abandon->operation_id) != 0 ||
        memcmp(before.operation.package_sha256, abandon->package_sha256,
               sizeof abandon->package_sha256) != 0 ||
        memcmp(before.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0)
        return ECONTAINER_SLOTS_CONFLICT;
    econtainer_slots_state_t abandoned = {0};
    result = econtainer_slots_abandon(&s_product.provider.io,
        &s_product.provider.geometry, before.sequence, s_product.boot_id,
        NULL, NULL, &abandoned);
    if (result != ECONTAINER_SLOTS_OK) return result;
    econtainer_slots_state_t readback = {0};
    result = econtainer_slots_load(&s_product.provider.io,
                                   &s_product.provider.geometry, &readback);
    if (result != ECONTAINER_SLOTS_OK ||
        readback.sequence != before.sequence + 1U ||
        readback.phase != ECONTAINER_SLOT_ABORTED ||
        memcmp(readback.operation.operation_id, abandon->operation_id,
               sizeof abandon->operation_id) != 0 ||
        memcmp(readback.operation.package_sha256, abandon->package_sha256,
               sizeof abandon->package_sha256) != 0)
        return ECONTAINER_SLOTS_UNCERTAIN;
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (!same_binding(&before.bindings[index], &readback.bindings[index]))
            return ECONTAINER_SLOTS_UNCERTAIN;
    }
    abandon->aborted_sequence = readback.sequence;
    return ECONTAINER_SLOTS_OK;
}

bool esp_base_container_product_abandon_prepared_package(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const uint8_t package_sha256[32], uint32_t *aborted_sequence)
{
    if (aborted_sequence != NULL) *aborted_sequence = 0U;
    prepared_abandon_context_t abandon = {.prepared_sequence = prepared_sequence};
    if (!esp_base_storage_claim_active(claim) || !policy_present() ||
        !s_product.provider_bound || !s_product.start_attempted ||
        s_product.trial_mode || s_product.uninstall_uncertain ||
        prepared_sequence == 0U || prepared_sequence == UINT32_MAX ||
        package_sha256 == NULL || aborted_sequence == NULL ||
        !decode_uuid(operation_id, abandon.operation_id)) return false;
    memcpy(abandon.package_sha256, package_sha256, 32);
    if (esp_base_container_with_firmware_set(claim,
            ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
            abandon_prepared_package, &abandon) != ECONTAINER_SLOTS_OK)
        return false;
    *aborted_sequence = abandon.aborted_sequence;
    return true;
}

typedef struct {
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t package_sha256[32];
    uint32_t expected_sequence;
    esp_base_container_uninstall_recovery_t outcome;
} uninstall_recovery_context_t;

static econtainer_slots_result_t reconcile_uninstall(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    uninstall_recovery_context_t *recovery = context;
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    if (!bindings_match_firmware_set(&state, firmware_set))
        return ECONTAINER_SLOTS_CONFLICT;
    const int index = binding_index(&state, firmware_set->running_firmware_sha256);
    if (index < 0) return ECONTAINER_SLOTS_CONFLICT;
    const econtainer_slot_binding_t *running = &state.bindings[index];
    if (state.sequence == recovery->expected_sequence &&
        (state.phase == ECONTAINER_SLOT_IDLE ||
         state.phase == ECONTAINER_SLOT_CONFIRMED ||
         (state.phase == ECONTAINER_SLOT_ABORTED &&
          !state.operation.firmware_transition)) &&
        (!state.operation.firmware_transition ||
         memcmp(state.operation.target_firmware_sha256,
                firmware_set->running_firmware_sha256, 32) == 0) &&
        memcmp(state.operation.operation_id, recovery->operation_id,
               sizeof recovery->operation_id) != 0 &&
        running->package_present &&
        memcmp(running->package_sha256, recovery->package_sha256, 32) == 0) {
        recovery->outcome = ESP_BASE_CONTAINER_UNINSTALL_NOT_COMMITTED;
        return ECONTAINER_SLOTS_OK;
    }
    if (state.sequence == recovery->expected_sequence + 1U &&
        state.phase == ECONTAINER_SLOT_CONFIRMED &&
        state.operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
        !state.operation.firmware_transition &&
        memcmp(state.operation.operation_id, recovery->operation_id,
               sizeof recovery->operation_id) == 0 &&
        memcmp(state.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) == 0 &&
        digest_zero(state.operation.package_sha256) &&
        state.operation.package_size_bytes == 0U &&
        state.operation.guest_abi_version == 0U &&
        state.operation.data_schema_version == 0U &&
        state.operation.slot == 0U &&
        !running->package_present && running->slot == 0U &&
        digest_zero(running->package_sha256) &&
        running->package_size_bytes == 0U &&
        running->guest_abi_version == 0U &&
        running->data_schema_version == 0U) {
        recovery->outcome = ESP_BASE_CONTAINER_UNINSTALL_RECOVERED;
        return ECONTAINER_SLOTS_OK;
    }
    return ECONTAINER_SLOTS_CONFLICT;
}

esp_base_container_uninstall_recovery_t esp_base_container_product_reconcile_uninstall(
    const esp_base_storage_claim_t *claim,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32])
{
    if (!policy_present() || !esp_base_storage_claim_active(claim) ||
        !s_product.provider_bound || s_product.trial_mode ||
        s_product.uninstall_uncertain || operation_id == NULL ||
        expected_package_sha256 == NULL || expected_sequence == 0U ||
        expected_sequence == UINT32_MAX || digest_zero(expected_package_sha256)) {
        return ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN;
    }
    uninstall_recovery_context_t recovery = {
        .expected_sequence = expected_sequence,
        .outcome = ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN,
    };
    if (!decode_uuid(operation_id, recovery.operation_id))
        return ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN;
    memcpy(recovery.package_sha256, expected_package_sha256, 32);
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, reconcile_uninstall,
        &recovery);
    return result == ECONTAINER_SLOTS_OK ? recovery.outcome :
           ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN;
}

static bool same_binding(const econtainer_slot_binding_t *left,
                         const econtainer_slot_binding_t *right)
{
    return left->present == right->present &&
        left->package_present == right->package_present && left->slot == right->slot &&
        memcmp(left->firmware_sha256, right->firmware_sha256, 32) == 0 &&
        memcmp(left->package_sha256, right->package_sha256, 32) == 0 &&
        left->package_size_bytes == right->package_size_bytes &&
        left->guest_abi_version == right->guest_abi_version &&
        left->data_schema_version == right->data_schema_version;
}

typedef struct {
    uint32_t expected_sequence;
    uint32_t resolved_sequence;
    esp_base_container_package_recovery_t outcome;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t package_sha256[32];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
} package_recovery_context_t;

static econtainer_slots_result_t recover_pending_package(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    package_recovery_context_t *recovery = context;
    econtainer_slots_state_t before = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &before);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (!bindings_match_firmware_set(&before, firmware_set))
        return ECONTAINER_SLOTS_CONFLICT;
    const bool same_operation = memcmp(before.operation.operation_id,
        recovery->operation_id, sizeof recovery->operation_id) == 0;
    if (before.sequence == recovery->expected_sequence && !same_operation &&
        (before.phase == ECONTAINER_SLOT_IDLE ||
         before.phase == ECONTAINER_SLOT_CONFIRMED ||
         before.phase == ECONTAINER_SLOT_ABORTED)) {
        econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
        econtainer_slots_state_t checked = {0};
        result = econtainer_slots_reconcile(&s_product.provider.io,
            &s_product.provider.geometry, firmware_set, &checked, &decision);
        if (result != ECONTAINER_SLOTS_OK ||
            decision != ECONTAINER_SLOT_BOOT_CONFIRMED ||
            checked.sequence != before.sequence) return ECONTAINER_SLOTS_CONFLICT;
        recovery->resolved_sequence = before.sequence;
        recovery->outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED;
        return ECONTAINER_SLOTS_OK;
    }
    if (!same_operation || before.operation.firmware_transition ||
        before.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        memcmp(before.operation.package_sha256, recovery->package_sha256, 32) != 0 ||
        memcmp(before.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0 ||
        before.sequence <= recovery->expected_sequence ||
        before.sequence > recovery->expected_sequence + 5U) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    const uint32_t steps = before.sequence - recovery->expected_sequence;
    if (before.phase == ECONTAINER_SLOT_CONFIRMED) {
        const int index = binding_index(&before,
            firmware_set->running_firmware_sha256);
        if (steps != 5U || index < 0 ||
            memcmp(before.operation.trial_boot_id, recovery->boot_id,
                   sizeof recovery->boot_id) == 0 ||
            !before.bindings[index].package_present ||
            memcmp(before.bindings[index].package_sha256,
                   recovery->package_sha256, 32) != 0)
            return ECONTAINER_SLOTS_CONFLICT;
        econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
        econtainer_slots_state_t checked = {0};
        result = econtainer_slots_reconcile(&s_product.provider.io,
            &s_product.provider.geometry, firmware_set, &checked, &decision);
        if (result != ECONTAINER_SLOTS_OK ||
            decision != ECONTAINER_SLOT_BOOT_CONFIRMED ||
            checked.phase != ECONTAINER_SLOT_CONFIRMED ||
            checked.sequence != before.sequence ||
            memcmp(checked.operation.operation_id, recovery->operation_id,
                   sizeof recovery->operation_id) != 0 ||
            !same_binding(&checked.bindings[index], &before.bindings[index]) ||
            !same_binding(&checked.bindings[1 - index],
                          &before.bindings[1 - index]))
            return ECONTAINER_SLOTS_CONFLICT;
        recovery->resolved_sequence = checked.sequence;
        recovery->outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED;
        return ECONTAINER_SLOTS_OK;
    }
    if (before.phase == ECONTAINER_SLOT_ABORTED) {
        if (steps < 2U) return ECONTAINER_SLOTS_CONFLICT;
        econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
        econtainer_slots_state_t checked = {0};
        result = econtainer_slots_reconcile(&s_product.provider.io,
            &s_product.provider.geometry, firmware_set, &checked, &decision);
        if (result != ECONTAINER_SLOTS_OK ||
            decision != ECONTAINER_SLOT_BOOT_CONFIRMED ||
            checked.sequence != before.sequence) return ECONTAINER_SLOTS_CONFLICT;
        recovery->resolved_sequence = before.sequence;
        recovery->outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED;
        return ECONTAINER_SLOTS_OK;
    }
    if (!((before.phase == ECONTAINER_SLOT_WRITING && steps == 1U) ||
          (before.phase == ECONTAINER_SLOT_PREPARED && steps == 2U) ||
          (before.phase == ECONTAINER_SLOT_TRIAL_STARTED && steps == 3U) ||
          (before.phase == ECONTAINER_SLOT_HEALTH_VERIFIED && steps == 4U))) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    if (before.phase >= ECONTAINER_SLOT_TRIAL_STARTED &&
        memcmp(before.operation.trial_boot_id, recovery->boot_id,
               sizeof recovery->boot_id) == 0) return ECONTAINER_SLOTS_BUSY;
    econtainer_slots_state_t abandoned = {0};
    result = econtainer_slots_abandon(&s_product.provider.io,
        &s_product.provider.geometry, before.sequence, recovery->boot_id,
        NULL, NULL, &abandoned);
    if (result != ECONTAINER_SLOTS_OK) return result;
    econtainer_slots_state_t readback = {0};
    result = econtainer_slots_load(&s_product.provider.io,
        &s_product.provider.geometry, &readback);
    if (result != ECONTAINER_SLOTS_OK ||
        readback.sequence != before.sequence + 1U ||
        readback.phase != ECONTAINER_SLOT_ABORTED ||
        readback.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        readback.operation.firmware_transition ||
        memcmp(readback.operation.operation_id, recovery->operation_id,
               sizeof recovery->operation_id) != 0 ||
        memcmp(readback.operation.package_sha256, recovery->package_sha256, 32) != 0) {
        return ECONTAINER_SLOTS_UNCERTAIN;
    }
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (!same_binding(&before.bindings[index], &readback.bindings[index]))
            return ECONTAINER_SLOTS_UNCERTAIN;
    }
    recovery->resolved_sequence = readback.sequence;
    recovery->outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED;
    return ECONTAINER_SLOTS_OK;
}

esp_base_container_package_recovery_t esp_base_container_product_recover_pending_package(
    const esp_base_storage_claim_t *claim, const char boot_id[37],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t package_sha256[32],
    uint32_t *resolved_sequence)
{
    if (resolved_sequence != NULL) *resolved_sequence = 0U;
    if (!esp_base_storage_claim_active(claim) || !policy_present() ||
        s_product.start_attempted || s_product.thread_joinable ||
        expected_sequence == 0U || expected_sequence > UINT32_MAX - 5U ||
        package_sha256 == NULL || resolved_sequence == NULL ||
        !ensure_provider(claim))
        return ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN;
    package_recovery_context_t recovery = {.expected_sequence = expected_sequence};
    if (!decode_uuid(boot_id, recovery.boot_id) ||
        !decode_uuid(operation_id, recovery.operation_id))
        return ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN;
    memcpy(recovery.package_sha256, package_sha256, 32);
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        recover_pending_package, &recovery);
    if (result != ECONTAINER_SLOTS_OK)
        return ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN;
    *resolved_sequence = recovery.resolved_sequence;
    return recovery.outcome;
}

static econtainer_slots_result_t uninstall_preflight(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    uninstall_context_t *uninstall = context;
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    const int index = binding_index(&state, firmware_set->running_firmware_sha256);
    if (state.sequence != uninstall->expected_sequence ||
        (state.phase != ECONTAINER_SLOT_IDLE &&
         state.phase != ECONTAINER_SLOT_CONFIRMED &&
         state.phase != ECONTAINER_SLOT_ABORTED) ||
        (state.operation.firmware_transition &&
         (state.phase == ECONTAINER_SLOT_ABORTED ||
          (state.phase == ECONTAINER_SLOT_CONFIRMED &&
           memcmp(state.operation.target_firmware_sha256,
                  firmware_set->running_firmware_sha256, 32) != 0))) ||
        memcmp(state.operation.operation_id, uninstall->operation_id,
               sizeof uninstall->operation_id) == 0 ||
        index < 0 || !state.bindings[index].package_present ||
        memcmp(state.bindings[index].package_sha256,
               uninstall->package_sha256, 32) != 0) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    memcpy(uninstall->running_firmware_sha256,
           firmware_set->running_firmware_sha256, 32);
    return ECONTAINER_SLOTS_OK;
}

/* Container invokes this under its slot lock. It may inspect the already
 * completed native stop, but must not enter another storage operation. */
static bool uninstall_instance_stopped(void *context,
                                       const econtainer_slot_binding_t *binding)
{
    const uninstall_context_t *uninstall = context;
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    return esp_base_storage_claim_active(uninstall->claim) &&
        !s_product.trial_mode && !s_product.thread_joinable &&
        s_product.native_reclaimed &&
        !atomic_load_explicit(&s_product.instance_active, memory_order_acquire) &&
        (result == ESP_BASE_CONTAINER_STOPPED || result == ESP_BASE_CONTAINER_BLOCKED) &&
        binding->present && binding->package_present &&
        memcmp(binding->firmware_sha256,
               uninstall->running_firmware_sha256, 32) == 0 &&
        memcmp(binding->package_sha256, uninstall->package_sha256, 32) == 0;
}

static econtainer_slots_result_t uninstall_confirmed_binding(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    uninstall_context_t *uninstall = context;
    econtainer_slots_state_t before = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &before);
    if (result != ECONTAINER_SLOTS_OK) return result;
    const int index = binding_index(&before, firmware_set->running_firmware_sha256);
    if (before.sequence != uninstall->expected_sequence || index < 0 ||
        !before.bindings[index].package_present ||
        memcmp(before.bindings[index].package_sha256,
               uninstall->package_sha256, 32) != 0) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    econtainer_slots_state_t committed = {0};
    result = econtainer_slots_uninstall(
        &s_product.provider.io, &s_product.provider.geometry,
        uninstall->expected_sequence, firmware_set, uninstall->operation_id,
        uninstall->package_sha256, uninstall_instance_stopped, uninstall,
        &committed);
    if (result != ECONTAINER_SLOTS_OK) return result;

    /* This load is separate from Container's commit readback and requires a
     * fresh provider read of the persisted blob before reopening this boot. */
    econtainer_slots_state_t readback = {0};
    result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &readback);
    if (result != ECONTAINER_SLOTS_OK) return ECONTAINER_SLOTS_UNCERTAIN;
    const econtainer_slot_binding_t *running = &readback.bindings[index];
    if (readback.sequence != uninstall->expected_sequence + 1U ||
        readback.phase != ECONTAINER_SLOT_CONFIRMED ||
        readback.operation.kind != ECONTAINER_SLOT_NO_PACKAGE ||
        readback.operation.firmware_transition ||
        memcmp(readback.operation.operation_id, uninstall->operation_id,
               sizeof uninstall->operation_id) != 0 ||
        memcmp(readback.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0 ||
        !digest_zero(readback.operation.package_sha256) ||
        readback.operation.package_size_bytes != 0U ||
        readback.operation.guest_abi_version != 0U ||
        readback.operation.data_schema_version != 0U ||
        readback.operation.slot != 0U ||
        !running->present || running->package_present || running->slot != 0U ||
        memcmp(running->firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0 ||
        !digest_zero(running->package_sha256) || running->package_size_bytes != 0U ||
        running->guest_abi_version != 0U || running->data_schema_version != 0U) {
        return ECONTAINER_SLOTS_UNCERTAIN;
    }
    for (unsigned other = 0; other < ECONTAINER_SLOT_BINDING_COUNT; ++other) {
        if ((int)other != index &&
            !same_binding(&before.bindings[other], &readback.bindings[other])) {
            return ECONTAINER_SLOTS_UNCERTAIN;
        }
    }
    return ECONTAINER_SLOTS_OK;
}

static bool reclaim_confirmed_for_uninstall(const esp_base_storage_claim_t *claim)
{
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    if (result == ESP_BASE_CONTAINER_RUNNING) {
        return esp_base_container_product_stop_confirmed(claim);
    }
    if (result == ESP_BASE_CONTAINER_BLOCKED && s_product.thread_joinable) {
        /* A worker that failed after publishing RUNNING may still be closing.
         * Join it before accepting its native cleanup proof. */
        atomic_store_explicit(&s_product.stop_requested, true, memory_order_release);
        if (xSemaphoreTake(s_product.stopped, pdMS_TO_TICKS(5000U)) != pdTRUE ||
            pthread_join(s_product.thread, NULL) != 0) return false;
        s_product.thread_joinable = false;
        s_product.claim = NULL;
    }
    return !s_product.thread_joinable && s_product.native_reclaimed &&
        !atomic_load_explicit(&s_product.instance_active, memory_order_acquire) &&
        (atomic_load_explicit(&s_product.result, memory_order_acquire) ==
             ESP_BASE_CONTAINER_STOPPED ||
         atomic_load_explicit(&s_product.result, memory_order_acquire) ==
             ESP_BASE_CONTAINER_BLOCKED);
}

esp_base_container_uninstall_result_t esp_base_container_product_uninstall(
    const esp_base_storage_claim_t *claim,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32])
{
    if (!policy_present()) return ESP_BASE_CONTAINER_UNINSTALL_NOT_CONFIGURED;
    if (!esp_base_storage_claim_active(claim) || s_product.trial_mode ||
        !s_product.provider_bound || s_product.ready == NULL ||
        !s_product.start_attempted || s_product.uninstall_uncertain) {
        return ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN;
    }
    uninstall_context_t uninstall = {.claim = claim,
                                     .expected_sequence = expected_sequence};
    if (operation_id == NULL || expected_package_sha256 == NULL ||
        expected_sequence == 0U || expected_sequence == UINT32_MAX ||
        digest_zero(expected_package_sha256) ||
        !decode_uuid(operation_id, uninstall.operation_id)) {
        return ESP_BASE_CONTAINER_UNINSTALL_REJECTED;
    }
    memcpy(uninstall.package_sha256, expected_package_sha256,
           sizeof uninstall.package_sha256);
    const econtainer_slots_result_t preflight = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, uninstall_preflight,
        &uninstall);
    if (preflight == ECONTAINER_SLOTS_CONFLICT) {
        return ESP_BASE_CONTAINER_UNINSTALL_REJECTED;
    }
    if (preflight != ECONTAINER_SLOTS_OK) {
        s_product.uninstall_uncertain = true;
        s_product.reopen_allowed = false;
        return ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN;
    }
    if (!reclaim_confirmed_for_uninstall(claim)) {
        s_product.uninstall_uncertain = true;
        s_product.reopen_allowed = false;
        return ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN;
    }
    s_product.reopen_allowed = false;
    const econtainer_slots_result_t committed = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, uninstall_confirmed_binding,
        &uninstall);
    if (committed != ECONTAINER_SLOTS_OK) {
        s_product.uninstall_uncertain = true;
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN result=%d", (int)committed);
        return ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN;
    }
    s_product.reopen_allowed = true;
    return ESP_BASE_CONTAINER_UNINSTALL_COMPLETE;
}
