// SPDX-License-Identifier: Apache-2.0
#include "esp_app_desc.h"
#include "esp_base_identity.h"
#include "eota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_base_protocol.h"
#include "esp_base_remote_config.h"
#include "esp_base_safety.h"
#include "esp_base_time.h"
#include "esp_base_ota_receipt.h"
#include "esp_base_ota_firmware.h"
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
#include "esp_frp_idf_flash_store.h"
#endif
#include "freertos/task.h"

#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void app_main(void);

static eota_state_t image_state, state_after_mark;
static esp_err_t inspect_result, nvs_result, identity_result, safety_result, config_result;
static esp_err_t protocol_result, mark_result, rollback_result, time_result;
static uint64_t now_ms, last_control_progress_ms;
static uint32_t control_progress_count;
static unsigned nvs_calls, config_load_calls, protocol_calls, mark_calls, rollback_calls;
static unsigned time_calls, ready_logs, recovery_logs;
static bool protocol_started, control_never_ready, control_stalls;
static bool control_exits_late, control_pauses_cross_window, ota_gate_pending, ota_available;
static esp_base_ota_receipt_result_t receipt_load_result, receipt_failure_result;
static esp_base_ota_receipt_result_t receipt_success_result, receipt_query_result;
static esp_base_ota_receipt_recovery_t receipt;
static eota_slots_t receipt_slots;
static eota_result_t receipt_observe_result, receipt_sha_result, receipt_retire_result;
static bool receipt_sha_mismatch, firmware_observation_ok, rollback_sha_mismatch;
static unsigned receipt_load_calls, receipt_observe_calls, receipt_sha_calls;
static unsigned receipt_retire_calls, receipt_failure_calls, receipt_success_calls;
static esp_base_storage_owner_t *storage_owner;
static eota_flash_io_t bound_flash_io;
static unsigned flash_io_bind_calls, ota_gate_clears;
static jmp_buf reboot_target;
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
static esp_base_storage_owner_t *scratch_io_owner;
static efrp_idf_flash_store_config_t scratch_config;
static bool scratch_bind_ok, scratch_recover_ok;
static unsigned scratch_bind_calls, scratch_recover_calls, scratch_erase_calls;
#endif

static bool storage_available(void)
{
    esp_base_storage_claim_t competing = {0};
    const bool available = esp_base_storage_claim(storage_owner, &competing);
    if (available) assert(esp_base_storage_release(&competing));
    return available;
}

static void reset_case(void)
{
    image_state = EOTA_STATE_PENDING_VERIFY;
    state_after_mark = EOTA_STATE_VALID;
    inspect_result = nvs_result = identity_result = safety_result = config_result = ESP_OK;
    protocol_result = mark_result = rollback_result = time_result = ESP_OK;
    now_ms = last_control_progress_ms = 0;
    control_progress_count = 0;
    nvs_calls = config_load_calls = protocol_calls = mark_calls = rollback_calls = 0;
    time_calls = ready_logs = recovery_logs = 0;
    protocol_started = control_never_ready = control_stalls = ota_gate_pending = false;
    ota_available = firmware_observation_ok = true;
    receipt_load_result = receipt_failure_result = receipt_success_result = receipt_query_result = ESP_BASE_OTA_RECEIPT_OK;
    receipt_observe_result = receipt_sha_result = receipt_retire_result = EOTA_UPDATE_OK;
    receipt_sha_mismatch = rollback_sha_mismatch = false;
    receipt = (esp_base_ota_receipt_recovery_t){
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .image_size_bytes = 512,
    };
    strcpy(receipt.operation_id, "00000000-0000-4000-8000-000000000002");
    memset(receipt.source_sha256, 0x41, 32);
    memset(receipt.candidate_sha256, 0x43, 32);
    receipt_slots = (eota_slots_t){
        .running_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .running_address_bytes = ESP_BASE_OTA_1_ADDRESS_BYTES,
        .boot_address_bytes = ESP_BASE_OTA_1_ADDRESS_BYTES,
        .target_address_bytes = ESP_BASE_OTA_0_ADDRESS_BYTES,
        .running_state = EOTA_STATE_PENDING_VERIFY,
        .target_state = EOTA_STATE_VALID,
    };
    receipt_load_calls = receipt_observe_calls = receipt_sha_calls = 0;
    receipt_retire_calls = receipt_failure_calls = receipt_success_calls = 0;
    control_exits_late = control_pauses_cross_window = false;
    ota_gate_clears = 0;
    storage_owner = NULL;
    bound_flash_io = (eota_flash_io_t){0};
    flash_io_bind_calls = 0;
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    scratch_io_owner = NULL;
    scratch_config = (efrp_idf_flash_store_config_t){0};
    scratch_bind_ok = scratch_recover_ok = true;
    scratch_bind_calls = scratch_recover_calls = scratch_erase_calls = 0;
#endif
}

#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
bool efrp_idf_flash_store_bind(efrp_idf_flash_store_t *provider,
                                const efrp_idf_flash_store_config_t *config)
{
    assert(provider != NULL && config != NULL &&
           !strcmp(config->partition_label, "frp_scratch") &&
           config->partition_type == ESP_PARTITION_TYPE_DATA &&
           config->partition_subtype == ESP_PARTITION_SUBTYPE_DATA_UNDEFINED &&
           config->partition_offset_bytes == CONFIG_ESP_BASE_FRP_SCRATCH_OFFSET &&
           config->partition_size_bytes == UINT32_C(0x10000) &&
           config->with_owner != NULL);
    ++scratch_bind_calls;
    scratch_io_owner = config->owner_context;
    scratch_config = *config;
    provider->config = *config;
    return scratch_bind_ok;
}

const efrp_aead_flash_store_t *efrp_idf_flash_store_callbacks(
    const efrp_idf_flash_store_t *provider)
{
    static efrp_aead_flash_store_t store;
    store.context = (void *)provider;
    return &store;
}

static efrp_result_t scratch_erase(void *context)
{
    assert(context != NULL);
    ++scratch_erase_calls;
    esp_base_storage_claim_t competing = {0};
    assert(scratch_io_owner != NULL &&
           !esp_base_storage_claim(scratch_io_owner, &competing));
    return scratch_recover_ok ? EFRP_OK : EFRP_STORAGE_ERROR;
}

static efrp_result_t scratch_during_ota(void *context)
{
    (void)context;
    assert(storage_owner != NULL && scratch_io_owner != storage_owner);
    esp_base_storage_claim_t competing = {0};
    assert(!esp_base_storage_claim(storage_owner, &competing));
    assert(!esp_base_storage_claim(scratch_io_owner, &competing));
    return EFRP_OK;
}

efrp_result_t efrp_aead_flash_store_recover(const efrp_aead_flash_store_t *store)
{
    assert(store != NULL && store->context != NULL);
    const efrp_idf_flash_store_t *provider = store->context;
    ++scratch_recover_calls;
    return provider->config.with_owner(provider->config.owner_context,
                                       scratch_erase, (void *)provider);
}
#endif

void test_log(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (strstr(line, "ESP_BASE_READY")) ++ready_logs;
    if (strstr(line, "ESP_BASE_OTA_RECOVERY_REQUIRED")) ++recovery_logs;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "injected";
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)now_ms * 1000;
}

static void maybe_control_progress(void)
{
    if (!protocol_started || control_never_ready || (control_stalls && now_ms >= 10000) ||
        (control_exits_late && now_ms > 29000) ||
        (control_pauses_cross_window && now_ms >= 29000 && now_ms < 33000)) {
        return;
    }
    if (control_progress_count == 0 || last_control_progress_ms != now_ms) {
        ++control_progress_count;
        last_control_progress_ms = now_ms;
    }
}

void vTaskDelay(TickType_t ticks)
{
    now_ms += ticks;
    maybe_control_progress();
}

esp_err_t eota_inspect(eota_current_t *current)
{
    esp_base_storage_claim_t competing = {0};
    assert(bound_flash_io.context != NULL &&
           !esp_base_storage_claim(bound_flash_io.context, &competing));
    current->running_partition = receipt_slots.running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1";
    current->state = inspect_result == ESP_OK ? image_state : EOTA_STATE_UNKNOWN;
    return inspect_result;
}
bool eota_available(void) { return ota_available; }
eota_policy_t esp_base_ota_policy(bool trusted_time)
{
    (void)trusted_time;
    return (eota_policy_t){0};
}
bool esp_base_ota_policy_bind_flash_io(eota_flash_io_t flash_io)
{
    assert(flash_io.acquire && flash_io.release && flash_io.context);
    bound_flash_io = flash_io;
    ++flash_io_bind_calls;
    return true;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *out)
{
    assert(device_id && out);
    ++receipt_load_calls;
    if (receipt_load_result == ESP_BASE_OTA_RECEIPT_OK) *out = receipt;
    return receipt_load_result;
}
eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *slots)
{
    assert(policy && slots);
    ++receipt_observe_calls;
    *slots = receipt_slots;
    return receipt_observe_result;
}
eota_result_t eota_sha256_running(const eota_policy_t *policy,
    uint32_t size_bytes, uint8_t digest[EOTA_SHA256_BYTES])
{
    assert(policy && digest && size_bytes == receipt.image_size_bytes);
    ++receipt_sha_calls;
    if (receipt_sha_result == EOTA_UPDATE_OK) {
        memcpy(digest, receipt.candidate_sha256, EOTA_SHA256_BYTES);
        if (receipt_sha_mismatch) digest[0] ^= 1U;
    }
    return receipt_sha_result;
}
eota_result_t eota_retire_inactive(const eota_policy_t *policy,
    uint8_t target_subtype, const uint8_t source_sha256[EOTA_SHA256_BYTES])
{
    assert(policy && target_subtype == receipt.target_subtype &&
           memcmp(source_sha256, receipt.source_sha256, EOTA_SHA256_BYTES) == 0);
    ++receipt_retire_calls;
    return receipt_retire_result;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error)
{
    assert(device_id && operation_id && error == EOTA_UPDATE_RESOURCE_FAILURE);
    ++receipt_failure_calls;
    return receipt_failure_result;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_success(
    const char *device_id)
{
    assert(device_id && !strcmp(device_id,
           "00000000-0000-4000-8000-000000000001"));
    assert(ota_gate_pending && image_state == EOTA_STATE_VALID);
    ++receipt_success_calls;
    return receipt_success_result;
}
const char *eota_state_name(eota_state_t state)
{
    return state == EOTA_STATE_PENDING_VERIFY ? "pending_verify" :
        state == EOTA_STATE_VALID ? "valid" : "unknown";
}
esp_err_t eota_confirm_pending(eota_current_t *current)
{
    assert(current->state == EOTA_STATE_PENDING_VERIFY);
    ++mark_calls;
    assert(ota_gate_pending);
    esp_base_storage_claim_t competing = {0};
    assert(storage_owner != NULL && !esp_base_storage_claim(storage_owner, &competing));
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    const uint64_t flash_wait_started_ms = now_ms;
    assert(scratch_config.with_owner(scratch_config.owner_context,
                                     scratch_during_ota, NULL) == EFRP_STORAGE_ERROR);
    /* The pending otadata write holds the short physical I/O claim. */
    assert(now_ms - flash_wait_started_ms >= 500U &&
           now_ms - flash_wait_started_ms <= 501U);
    assert(!esp_base_storage_claim(storage_owner, &competing));
#endif
    image_state = state_after_mark;
    current->state = image_state;
    return image_state == EOTA_STATE_VALID ? ESP_OK :
        mark_result == ESP_OK ? ESP_ERR_INVALID_STATE : mark_result;
}
esp_err_t eota_reject_pending(eota_current_t *current)
{
    assert(current->state == EOTA_STATE_PENDING_VERIFY);
    ++rollback_calls;
    assert(ota_gate_pending);
    if (storage_owner != NULL) {
        esp_base_storage_claim_t competing = {0};
        assert(!esp_base_storage_claim(storage_owner, &competing));
    }
    if (rollback_result == ESP_OK) {
        image_state = EOTA_STATE_INVALID;
        longjmp(reboot_target, 1);
    }
    return rollback_result;
}

esp_err_t nvs_flash_init(void)
{
    esp_base_storage_claim_t competing = {0};
    assert(bound_flash_io.context != NULL &&
           !esp_base_storage_claim(bound_flash_io.context, &competing));
    ++nvs_calls;
    return nvs_result;
}

esp_err_t esp_base_identity_read(esp_base_identity_t *identity)
{
    esp_base_storage_claim_t competing = {0};
    assert(bound_flash_io.context != NULL &&
           !esp_base_storage_claim(bound_flash_io.context, &competing));
    if (identity_result == ESP_OK) {
        (void)snprintf(identity->device_id, sizeof identity->device_id,
                       "00000000-0000-4000-8000-000000000001");
        identity->model = "esp32c3";
        identity->revision = 4;
        identity->flash_size_bytes = 4 * 1024 * 1024;
    }
    return identity_result;
}

esp_err_t esp_base_safety_start(esp_base_safety_t *safety)
{
    safety->reset_reason = "power_on";
    return safety_result;
}

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    *config = (esp_base_remote_config_t){0};
    if (config_result == ESP_OK) config->revision = 7;
    return config_result;
}

esp_err_t esp_base_protocol_load_config(uint32_t *revision,
                                        esp_base_storage_owner_t *flash_io_owner)
{
    assert(flash_io_owner == bound_flash_io.context);
    ++config_load_calls;
    esp_base_remote_config_t config;
    const esp_err_t result = esp_base_remote_config_load(&config);
    if (result == ESP_OK) *revision = config.revision;
    return result;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.version = "test"};
    return &app;
}

const char *esp_get_idf_version(void)
{
    return "test";
}

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{
    assert(context != NULL);
    assert(context->storage_owner != NULL);
    assert(context->flash_io_owner != NULL);
    assert(flash_io_bind_calls == 1 &&
           bound_flash_io.context == context->flash_io_owner);
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    assert(context->frp_flash_store != NULL);
#else
    assert(context->frp_flash_store == NULL);
#endif
    assert(config_load_calls == 1 && config_result == ESP_OK);
    storage_owner = context->storage_owner;
    esp_base_storage_claim_t competing = {0};
    assert(!esp_base_storage_claim(storage_owner, &competing));
    assert(ota_gate_pending);
    ++protocol_calls;
    protocol_started = protocol_result == ESP_OK;
    maybe_control_progress();
    return protocol_result;
}

esp_err_t esp_base_time_start(const char *server)
{
    assert(server != NULL && !strcmp(server, "time.example.invalid"));
    ++time_calls;
    return time_result;
}
bool esp_base_protocol_control_healthy(void)
{
    return protocol_started && control_progress_count != 0 &&
           now_ms - last_control_progress_ms <= 5000;
}
uint32_t esp_base_protocol_control_progress_count(void) { return control_progress_count; }
void esp_base_protocol_set_ota_verification_pending(bool pending)
{
    ota_gate_pending = pending;
    if (!pending) ++ota_gate_clears;
}
esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation, const eota_prepared_t *prepared,
    esp_base_ota_firmware_set_t *set)
{
    assert(prepared == NULL && set != NULL);
    if (!firmware_observation_ok ||
        image_state != (observation == ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL ?
                        EOTA_STATE_PENDING_VERIFY : EOTA_STATE_VALID))
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    *set = (esp_base_ota_firmware_set_t){.bootable_count = 2};
    memcpy(set->running_firmware_sha256, receipt.candidate_sha256, 32);
    memcpy(set->bootable_firmware_sha256[0], receipt.candidate_sha256, 32);
    memcpy(set->bootable_firmware_sha256[1], receipt.source_sha256, 32);
    if (rollback_sha_mismatch) set->bootable_firmware_sha256[1][0] ^= 1U;
    return ESP_BASE_OTA_FIRMWARE_OK;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    assert(device_id && operation_id && !worker_active && view);
    view->state = receipt_slots.running_subtype == receipt.source_subtype &&
        receipt_slots.running_state == EOTA_STATE_VALID &&
        receipt.status == ESP_BASE_OTA_RECEIPT_FAILED ?
        ESP_BASE_OTA_OPERATION_FAILED : ESP_BASE_OTA_OPERATION_UNKNOWN;
    return receipt_query_result;
}
static bool rebooted(void)
{
    if (setjmp(reboot_target) != 0) return true;
    app_main();
    return false;
}
static void source_boot(void)
{
    image_state = EOTA_STATE_VALID;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.source_subtype;
    receipt_slots.target_subtype = receipt.target_subtype;
    receipt_slots.running_address_bytes = receipt_slots.boot_address_bytes = ESP_BASE_OTA_0_ADDRESS_BYTES;
    receipt_slots.target_address_bytes = ESP_BASE_OTA_1_ADDRESS_BYTES;
    receipt_slots.running_state = EOTA_STATE_VALID;
    receipt_slots.target_state = EOTA_STATE_INVALID;
}

int main(void)
{
    reset_case();
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(now_ms >= 30000 && now_ms < 31000 && receipt_success_calls == 1);
    assert(!ota_gate_pending && ota_gate_clears == 1 && storage_available());
    assert(bound_flash_io.acquire(bound_flash_io.context));
    assert(bound_flash_io.release(bound_flash_io.context));
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    assert(scratch_bind_calls == 1 && scratch_recover_calls == 1 && scratch_erase_calls == 1);
    reset_case(); scratch_bind_ok = false;
    assert(!rebooted() && protocol_calls == 0 && mark_calls == 0 && ready_logs == 0);
    reset_case(); scratch_recover_ok = false;
    assert(!rebooted() && protocol_calls == 0 && mark_calls == 0 && ready_logs == 0);
#endif
    reset_case(); time_result = ESP_FAIL;
    assert(!rebooted() && mark_calls == 1 && ready_logs == 1 && time_calls == 1);
    reset_case(); control_never_ready = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms == 5000);
    reset_case(); control_stalls = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms >= 15000);
    reset_case(); control_exits_late = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms > 30000);
    reset_case(); control_pauses_cross_window = true;
    assert(!rebooted() && mark_calls == 1 && ready_logs == 1 && now_ms >= 33000 && now_ms < 34000);
    reset_case(); state_after_mark = EOTA_STATE_PENDING_VERIFY; mark_result = ESP_FAIL;
    assert(rebooted() && mark_calls == 1 && rollback_calls == 1 && ready_logs == 0);
    reset_case(); state_after_mark = EOTA_STATE_VALID; mark_result = ESP_FAIL;
    assert(!rebooted() && mark_calls == 1 && ready_logs == 1);

    for (unsigned stage = 0; stage < 5; ++stage) {
        reset_case();
        switch (stage) {
        case 0: nvs_result = ESP_FAIL; break;
        case 1: identity_result = ESP_FAIL; break;
        case 2: safety_result = ESP_FAIL; break;
        case 3: config_result = ESP_FAIL; break;
        case 4: protocol_result = ESP_FAIL; break;
        }
        assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && ready_logs == 0);
    }
    reset_case(); nvs_result = ESP_FAIL; rollback_result = ESP_ERR_OTA_ROLLBACK_FAILED;
    assert(!rebooted() && rollback_calls == 1 && recovery_logs == 1 && ota_gate_pending);

    /* Exact original intent and verified source/candidate are mandatory. */
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        reset_case();
        switch (scenario) {
        case 0: receipt_load_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN; break;
        case 1: receipt_observe_result = EOTA_UPDATE_RESOURCE_FAILURE; break;
        case 2: receipt_sha_mismatch = true; break;
        case 3: rollback_sha_mismatch = true; break;
        case 4: firmware_observation_ok = false; break;
        case 5: receipt_slots.boot_subtype = receipt.source_subtype; break;
        }
        assert(rebooted() && mark_calls == 0 && receipt_success_calls == 0 && ready_logs == 0);
    }
    reset_case(); receipt_load_result = ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    assert(rebooted() && mark_calls == 0 && ready_logs == 0);
    reset_case(); receipt_success_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    assert(!rebooted() && mark_calls == 1 && receipt_success_calls == 1 &&
           ready_logs == 0 && ota_gate_pending && !storage_available());

    reset_case(); source_boot();
    assert(!rebooted() && receipt_retire_calls == 1 && receipt_failure_calls == 1 && ready_logs == 1);
    assert(mark_calls == 0 && receipt_success_calls == 0 && storage_available());
    reset_case(); source_boot(); receipt_retire_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    assert(!rebooted() && receipt_failure_calls == 0 && ready_logs == 0 && ota_gate_pending && !storage_available());
    reset_case(); source_boot(); receipt_failure_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    assert(!rebooted() && receipt_failure_calls == 1 && ready_logs == 0 && ota_gate_pending && !storage_available());
    reset_case(); source_boot(); receipt.status = ESP_BASE_OTA_RECEIPT_FAILED;
    assert(!rebooted() && ready_logs == 1 && receipt_retire_calls == 0 && receipt_failure_calls == 0);
    reset_case(); receipt.status = ESP_BASE_OTA_RECEIPT_FAILED;
    assert(rebooted() && ready_logs == 0 && mark_calls == 0);
    reset_case(); source_boot(); receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(!rebooted() && ready_logs == 0 && receipt_retire_calls == 0);
    reset_case(); image_state = receipt_slots.running_state = EOTA_STATE_VALID;
    assert(!rebooted() && mark_calls == 0 && receipt_success_calls == 1 && ready_logs == 1);
    reset_case(); image_state = receipt_slots.running_state = EOTA_STATE_VALID;
    receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(!rebooted() && mark_calls == 0 && receipt_success_calls == 0 && ready_logs == 1);
    reset_case(); source_boot(); receipt_load_result = ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    assert(!rebooted() && mark_calls == 0 && ready_logs == 1);
    reset_case(); source_boot(); ota_available = false;
    assert(!rebooted() && receipt_load_calls == 0 && mark_calls == 0 && ready_logs == 1);
    puts("  ota_startup passed (firmware-only recovery, 30-second local progress, rollback, independent scratch I/O)");
}
