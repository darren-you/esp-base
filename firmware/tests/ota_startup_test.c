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
#include "esp_base_container_product.h"
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

static eota_state_t image_state;
static eota_state_t state_after_mark;
static esp_err_t inspect_result, nvs_result, identity_result, safety_result, config_result;
static esp_err_t protocol_result, mark_result, rollback_result, time_result;
static uint64_t now_ms;
static uint64_t last_control_progress_ms;
static uint32_t control_progress_count;
static unsigned nvs_calls, config_load_calls, protocol_calls, mark_calls, rollback_calls, time_calls, ready_logs, recovery_logs;
static bool protocol_started, control_never_ready, control_stalls;
static bool product_ledger_ready;
static unsigned product_ledger_prepare_calls;
static bool product_recovery_ready;
static unsigned product_recovery_calls;
static bool control_exits_late, control_pauses_cross_window;
static bool ota_gate_pending;
static bool ota_available, without_receipt_ok;
static bool container_configured, container_health_ok, container_confirm_ok, container_stop_ok;
static esp_base_ota_receipt_result_t receipt_load_result, receipt_failure_result,
    receipt_success_result;
static esp_base_ota_receipt_recovery_t receipt;
static eota_slots_t receipt_slots;
static eota_result_t receipt_observe_result, receipt_sha_result, receipt_retire_result;
static bool receipt_sha_mismatch;
static esp_base_container_retire_result_t container_recover_result;
static unsigned receipt_load_calls, receipt_observe_calls, receipt_sha_calls;
static unsigned receipt_retire_calls, container_recover_calls,
    container_selected_calls, without_receipt_calls,
    receipt_failure_calls, receipt_success_calls;
static bool container_selected_ok;
static esp_base_container_boot_result_t container_boot_result;
static unsigned container_boot_calls, container_trial_calls, container_health_calls;
static unsigned container_confirm_calls, container_stop_calls;
static unsigned confirmed_stop_calls;
static esp_base_storage_owner_t *storage_owner;
static esp_base_storage_owner_t *container_flash_io_owner;
static eota_flash_io_t bound_flash_io;
static unsigned flash_io_bind_calls;
static unsigned ota_gate_clears;
static jmp_buf reboot_target;
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
static esp_base_storage_owner_t *scratch_io_owner;
static efrp_idf_flash_store_config_t scratch_config;
static bool scratch_bind_ok, scratch_recover_ok;
static unsigned scratch_bind_calls, scratch_recover_calls, scratch_erase_calls;
#endif

static void reset_case(void)
{
    image_state = EOTA_STATE_PENDING_VERIFY;
    state_after_mark = EOTA_STATE_VALID;
    inspect_result = nvs_result = identity_result = safety_result = config_result = ESP_OK;
    protocol_result = mark_result = rollback_result = time_result = ESP_OK;
    now_ms = last_control_progress_ms = 0;
    control_progress_count = 0;
    nvs_calls = config_load_calls = protocol_calls = mark_calls = rollback_calls = time_calls = ready_logs = recovery_logs = 0;
    protocol_started = control_never_ready = control_stalls = ota_gate_pending = false;
    product_ledger_ready = true;
    product_ledger_prepare_calls = 0;
    product_recovery_ready = true;
    product_recovery_calls = 0;
    ota_available = without_receipt_ok = true;
    container_configured = false;
    receipt_load_result = ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    receipt_failure_result = ESP_BASE_OTA_RECEIPT_OK;
    receipt_success_result = ESP_BASE_OTA_RECEIPT_OK;
    receipt_observe_result = receipt_sha_result = receipt_retire_result = EOTA_UPDATE_OK;
    receipt_sha_mismatch = false;
    container_recover_result = ESP_BASE_CONTAINER_RETIRE_COMPLETE;
    receipt = (esp_base_ota_receipt_recovery_t){0};
    receipt_slots = (eota_slots_t){0};
    receipt_load_calls = receipt_observe_calls = receipt_sha_calls = 0;
    receipt_retire_calls = container_recover_calls = container_selected_calls = 0;
    without_receipt_calls = 0;
    receipt_failure_calls = receipt_success_calls = 0;
    container_selected_ok = true;
    container_health_ok = container_confirm_ok = container_stop_ok = true;
    container_boot_result = ESP_BASE_CONTAINER_NOT_CONFIGURED;
    container_boot_calls = container_trial_calls = container_health_calls = 0;
    container_confirm_calls = container_stop_calls = 0;
    confirmed_stop_calls = 0;
    control_exits_late = control_pauses_cross_window = false;
    ota_gate_clears = 0;
    storage_owner = NULL;
    bound_flash_io = (eota_flash_io_t){0};
    flash_io_bind_calls = 0;
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    scratch_io_owner = NULL;
    scratch_config = (efrp_idf_flash_store_config_t){0};
    scratch_bind_ok = scratch_recover_ok = true;
    scratch_bind_calls = scratch_recover_calls = scratch_erase_calls = 0U;
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
    current->running_partition = "ota_1";
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

bool esp_base_protocol_prepare_product_ledger(const esp_base_storage_claim_t *claim,
                                              bool product_empty)
{
    assert(esp_base_storage_claim_active(claim) &&
           product_empty == (container_boot_result == ESP_BASE_CONTAINER_EMPTY));
    assert(ota_gate_pending);
    ++product_ledger_prepare_calls;
    return product_ledger_ready;
}

bool esp_base_protocol_recover_product_package(const esp_base_storage_claim_t *claim)
{
    assert(esp_base_storage_claim_active(claim) && container_boot_calls == 0U &&
           ota_gate_pending);
    ++product_recovery_calls;
    return product_recovery_ready;
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

uint32_t esp_base_protocol_control_progress_count(void)
{
    return control_progress_count;
}

void esp_base_protocol_set_ota_verification_pending(bool pending)
{
    ota_gate_pending = pending;
    if (!pending) ++ota_gate_clears;
}

bool esp_base_container_product_configured(void)
{
    return container_configured;
}

bool esp_base_container_product_without_ota_receipt(
    const esp_base_storage_claim_t *claim)
{
    assert(esp_base_storage_claim_active(claim));
    ++without_receipt_calls;
    return without_receipt_ok;
}

bool esp_base_container_product_reconcile_selected_ota(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *candidate,
    eota_state_t running_state)
{
    assert(esp_base_storage_claim_active(claim) && candidate != NULL &&
           !strcmp(candidate->operation_id, receipt.operation_id) &&
           (running_state == EOTA_STATE_PENDING_VERIFY ||
            running_state == EOTA_STATE_VALID));
    ++container_selected_calls;
    return container_selected_ok;
}

esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *recovery,
    const char boot_id[37])
{
    assert(esp_base_storage_claim_active(claim) &&
           recovery != NULL &&
           recovery->container_enabled == receipt.container_enabled &&
           recovery->container_sequence == receipt.container_sequence &&
           memcmp(recovery->source_sha256, receipt.source_sha256, 32) == 0 &&
           memcmp(recovery->inactive_sha256, receipt.inactive_sha256, 32) == 0 &&
           memcmp(recovery->candidate_sha256, receipt.candidate_sha256, 32) == 0 &&
           memcmp(recovery, &receipt, sizeof receipt) == 0 &&
           !strcmp(recovery->operation_id, receipt.operation_id) &&
           boot_id && boot_id[0] == '3');
    ++container_recover_calls;
    return container_recover_result;
}

const char *esp_base_protocol_boot_id(void)
{
    assert(protocol_started);
    return "33333333-3333-4333-8333-333333333333";
}

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    ++container_boot_calls;
    assert(container_flash_io_owner != NULL && container_flash_io_owner != storage_owner);
    assert(boot_id != NULL && boot_id[0] == '3');
    assert(esp_base_storage_claim_active(claim));
    assert(ota_gate_pending);
    if (receipt_load_result == ESP_BASE_OTA_RECEIPT_OK &&
        receipt.status == ESP_BASE_OTA_RECEIPT_PREPARED &&
        receipt.package_mode != ESP_BASE_OTA_NO_PACKAGE) {
        assert(receipt_retire_calls == 1 && container_recover_calls == 1 &&
               receipt_failure_calls == 1);
    }
    esp_base_storage_claim_t competing = {0};
    assert(!esp_base_storage_claim(storage_owner, &competing));
    return container_boot_result;
}

void esp_base_container_product_set_flash_io_owner(esp_base_storage_owner_t *owner)
{
    assert(owner != NULL && owner != storage_owner);
    container_flash_io_owner = owner;
}

esp_base_container_boot_result_t esp_base_container_product_start_trial(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    assert(container_configured && esp_base_storage_claim_active(claim));
    assert(boot_id != NULL && boot_id[0] == '3');
    assert(mark_calls == 0);
    ++container_trial_calls;
    return container_boot_result;
}

bool esp_base_container_product_mark_healthy(const esp_base_storage_claim_t *claim)
{
    assert(container_trial_calls == 1 && esp_base_storage_claim_active(claim));
    assert(mark_calls == 0 && now_ms >= 30000);
    ++container_health_calls;
    return container_health_ok;
}

bool esp_base_container_product_confirm_firmware(const esp_base_storage_claim_t *claim)
{
    assert(container_health_calls == 1 && mark_calls == 1 &&
           image_state == EOTA_STATE_VALID && esp_base_storage_claim_active(claim));
    ++container_confirm_calls;
    return container_confirm_ok;
}

bool esp_base_container_product_stop_trial(const esp_base_storage_claim_t *claim)
{
    assert(esp_base_storage_claim_active(claim));
    ++container_stop_calls;
    return container_stop_ok;
}

bool esp_base_container_product_stop_confirmed(const esp_base_storage_claim_t *claim)
{
    assert(esp_base_storage_claim_active(claim));
    ++confirmed_stop_calls;
    return container_stop_ok;
}

static bool rebooted(void)
{
    if (setjmp(reboot_target) == 0) {
        app_main();
        return false;
    }
    return true;
}

static void interrupted_receipt(bool enabled)
{
    receipt_load_result = ESP_BASE_OTA_RECEIPT_OK;
    receipt = (esp_base_ota_receipt_recovery_t){
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .image_size_bytes = 4096,
        .container_enabled = enabled,
        .container_sequence = enabled ? 7U : 0U,
    };
    strcpy(receipt.operation_id, "44444444-4444-4444-8444-444444444444");
    receipt.source_sha256[0] = 0xa0;
    receipt.inactive_sha256[0] = 0xb0;
    receipt.candidate_sha256[0] = 0xc0;
    receipt_slots = (eota_slots_t){
        .running_subtype = receipt.source_subtype,
        .boot_subtype = receipt.source_subtype,
        .target_subtype = receipt.target_subtype,
        .running_address_bytes = 0x20000,
        .boot_address_bytes = 0x20000,
        .running_state = EOTA_STATE_VALID,
        .target_state = EOTA_STATE_INVALID,
    };
}

static void selected_receipt(bool enabled, eota_state_t running_state)
{
    interrupted_receipt(enabled);
    receipt_slots.running_subtype = receipt.target_subtype;
    receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = running_state;
    image_state = running_state;
}

static void interrupted_package_receipt(esp_base_ota_package_mode_t mode)
{
    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    interrupted_receipt(true);
    image_state = EOTA_STATE_VALID;
    receipt.package_mode = mode;
    receipt.source_package_present = true;
    memset(receipt.source_package_sha256, 0x41, 32);
    memset(receipt.package_sha256, mode == ESP_BASE_OTA_PACKAGE_REUSE ? 0x41 : 0x42, 32);
    memset(receipt.trial_event_sha256, 0x43, 32);
    receipt.source_package_size_bytes = receipt.package_size_bytes = 1024;
    receipt.source_guest_abi_version = receipt.guest_abi_version = 2;
    receipt.source_data_schema_version = receipt.data_schema_version = 1;
}

int main(void)
{
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    reset_case();
    scratch_bind_ok = false;
    assert(!rebooted() && scratch_bind_calls == 1U &&
           scratch_recover_calls == 0U && scratch_erase_calls == 0U &&
           nvs_calls == 0U && mark_calls == 0U);

    reset_case();
    scratch_recover_ok = false;
    assert(!rebooted() && scratch_bind_calls == 1U &&
           scratch_recover_calls == 1U && scratch_erase_calls == 1U &&
           nvs_calls == 0U && mark_calls == 0U);
    esp_base_storage_claim_t after_recover_failure = {0};
    assert(esp_base_storage_claim(scratch_io_owner, &after_recover_failure));
    assert(esp_base_storage_release(&after_recover_failure));
#endif
    reset_case();
    image_state = EOTA_STATE_VALID;
    nvs_result = ESP_FAIL;
    assert(!rebooted() && rollback_calls == 0 && recovery_logs == 0);

    reset_case();
    inspect_result = ESP_FAIL;
    assert(!rebooted() && nvs_calls == 0 && rollback_calls == 0 && recovery_logs == 1);

    reset_case();
    nvs_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 0 && mark_calls == 0);

    reset_case();
    identity_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 0);

    reset_case();
    safety_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 0);

    reset_case();
    config_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && config_load_calls == 1 && protocol_calls == 0);

    reset_case();
    protocol_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 1 && time_calls == 0);

    reset_case();
    time_result = ESP_FAIL;
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(time_calls == 1 && !ota_gate_pending);
    esp_base_storage_claim_t after_ready = {0};
    assert(esp_base_storage_claim(storage_owner, &after_ready));
    assert(esp_base_storage_release(&after_ready));
    assert(bound_flash_io.acquire(bound_flash_io.context));
    assert(bound_flash_io.release(bound_flash_io.context));

    reset_case();
    control_never_ready = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms == 5000);

    reset_case();
    control_stalls = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms >= 15000);

    reset_case();
    control_exits_late = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms > 30000);

    reset_case();
    control_pauses_cross_window = true;
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(now_ms >= 33000 && now_ms < 34000);

    reset_case();
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(!ota_gate_pending && ota_gate_clears == 1);
    assert(now_ms >= 30000 && image_state == EOTA_STATE_VALID);

    reset_case();
    mark_result = ESP_FAIL;
    state_after_mark = EOTA_STATE_PENDING_VERIFY;
    assert(rebooted() && mark_calls == 1 && rollback_calls == 1 && ready_logs == 0);

    reset_case();
    mark_result = ESP_FAIL;
    state_after_mark = EOTA_STATE_VALID;
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0);
    assert(ready_logs == 1 && recovery_logs == 0 && image_state == EOTA_STATE_VALID);
    assert(!ota_gate_pending && ota_gate_clears == 1);

    reset_case();
    nvs_result = ESP_FAIL;
    rollback_result = ESP_ERR_OTA_ROLLBACK_FAILED;
    assert(!rebooted() && rollback_calls == 1 && recovery_logs == 1);
    assert(image_state == EOTA_STATE_PENDING_VERIFY && ota_gate_pending);

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_BLOCKED;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 &&
           container_trial_calls == 1 && container_stop_calls == 1 &&
           container_boot_calls == 0 && ready_logs == 0);

    reset_case();
    container_configured = true;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    receipt.package_mode = ESP_BASE_OTA_PACKAGE_REUSE;
    assert(rebooted() && rollback_calls == 1 && receipt_observe_calls == 1 &&
           container_selected_calls == 0 && container_trial_calls == 0 &&
           mark_calls == 0 && ready_logs == 0);

    for (esp_base_ota_package_mode_t mode = ESP_BASE_OTA_PACKAGE_REUSE;
         mode <= ESP_BASE_OTA_PACKAGE_WRITE; ++mode) {
        interrupted_package_receipt(mode);
        assert(!rebooted() && receipt_retire_calls == 1 &&
               container_recover_calls == 1 && receipt_failure_calls == 1 &&
               container_boot_calls == 1 && ready_logs == 1 &&
               mark_calls == 0 && !ota_gate_pending);
        esp_base_storage_claim_t recovered = {0};
        assert(esp_base_storage_claim(storage_owner, &recovered));
        assert(esp_base_storage_release(&recovered));

        interrupted_package_receipt(mode);
        receipt_retire_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        assert(!rebooted() && receipt_retire_calls == 1 &&
               container_recover_calls == 0 && receipt_failure_calls == 0 &&
               container_boot_calls == 0 && ota_gate_pending);

        interrupted_package_receipt(mode);
        container_recover_result = ESP_BASE_CONTAINER_RETIRE_UNCERTAIN;
        assert(!rebooted() && container_recover_calls == 1 &&
               receipt_failure_calls == 0 && container_boot_calls == 0 &&
               ota_gate_pending);

        interrupted_package_receipt(mode);
        receipt_failure_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
        assert(!rebooted() && receipt_failure_calls == 1 &&
               container_boot_calls == 0 && ota_gate_pending);
        esp_base_storage_claim_t uncertain = {0};
        assert(!esp_base_storage_claim(storage_owner, &uncertain));

        interrupted_package_receipt(mode);
        container_configured = false;
        assert(!rebooted() && receipt_retire_calls == 0 &&
               container_recover_calls == 0 && receipt_failure_calls == 0 &&
               container_boot_calls == 0 && ota_gate_pending);

        interrupted_package_receipt(mode);
        receipt.status = ESP_BASE_OTA_RECEIPT_FAILED;
        assert(!rebooted() && without_receipt_calls == 1 &&
               receipt_retire_calls == 0 && receipt_failure_calls == 0 &&
               ready_logs == 1);

        interrupted_package_receipt(mode);
        receipt.status = ESP_BASE_OTA_RECEIPT_FAILED;
        without_receipt_ok = false;
        assert(!rebooted() && receipt_retire_calls == 0 &&
               container_boot_calls == 0 && ready_logs == 0 && ota_gate_pending);
    }

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    assert(!rebooted() && container_trial_calls == 1 &&
           container_health_calls == 1 && mark_calls == 1 &&
           container_confirm_calls == 1 && container_boot_calls == 0 &&
           container_stop_calls == 0 && ready_logs == 1);
    esp_base_storage_claim_t trial_competitor = {0};
    assert(esp_base_storage_claim(storage_owner, &trial_competitor));
    assert(esp_base_storage_release(&trial_competitor));

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    assert(!rebooted() && container_health_calls == 1 &&
           container_confirm_calls == 1 && ready_logs == 1);

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    container_health_ok = false;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    assert(rebooted() && container_health_calls == 1 && mark_calls == 0 &&
           container_stop_calls == 1 && rollback_calls == 1);

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    container_stop_ok = false;
    control_never_ready = true;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    assert(!rebooted() && container_stop_calls == 1 && rollback_calls == 0 &&
           recovery_logs >= 1 && ready_logs == 0);

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    container_confirm_ok = false;
    selected_receipt(true, EOTA_STATE_PENDING_VERIFY);
    assert(!rebooted() && container_confirm_calls == 1 &&
           container_stop_calls == 1 && rollback_calls == 0 &&
           recovery_logs >= 1 && ready_logs == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    assert(!rebooted() && container_boot_calls == 1 &&
           product_recovery_calls == 1 && product_ledger_prepare_calls == 1 &&
           ready_logs == 1);
    esp_base_storage_claim_t running_competitor = {0};
    assert(esp_base_storage_claim(storage_owner, &running_competitor));
    assert(esp_base_storage_release(&running_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    product_ledger_ready = false;
    assert(!rebooted() && product_ledger_prepare_calls == 1U &&
           confirmed_stop_calls == 1U && ready_logs == 0U && ota_gate_pending);
    esp_base_storage_claim_t running_ledger_competitor = {0};
    assert(!esp_base_storage_claim(storage_owner, &running_ledger_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    product_recovery_ready = false;
    assert(!rebooted() && product_recovery_calls == 1U &&
           container_boot_calls == 0U && product_ledger_prepare_calls == 0U &&
           ready_logs == 0U && ota_gate_pending);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    assert(!rebooted() && container_boot_calls == 1 &&
           product_ledger_prepare_calls == 1 && ready_logs == 1);
    esp_base_storage_claim_t empty_competitor = {0};
    assert(esp_base_storage_claim(storage_owner, &empty_competitor));
    assert(esp_base_storage_release(&empty_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    product_ledger_ready = false;
    assert(!rebooted() && container_boot_calls == 1 &&
           product_ledger_prepare_calls == 1 && ready_logs == 0 &&
           ota_gate_pending);
    esp_base_storage_claim_t ledger_competitor = {0};
    assert(!esp_base_storage_claim(storage_owner, &ledger_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    without_receipt_ok = false; /* ECS2 still contains a firmware transition. */
    assert(!rebooted() && without_receipt_calls == 1 &&
           container_boot_calls == 0 && ready_logs == 0 && ota_gate_pending);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    without_receipt_ok = false;
    receipt_load_result = ESP_BASE_OTA_RECEIPT_OK;
    receipt.status = ESP_BASE_OTA_RECEIPT_FAILED;
    receipt.container_enabled = true;
    assert(!rebooted() && without_receipt_calls == 1 &&
           container_boot_calls == 0 && ready_logs == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    ota_available = false;
    without_receipt_ok = false;
    assert(!rebooted() && without_receipt_calls == 1 &&
           receipt_load_calls == 0 && container_boot_calls == 0 && ready_logs == 0);

    reset_case();
    container_configured = true;
    without_receipt_ok = false;
    assert(rebooted() && without_receipt_calls == 1 &&
           container_trial_calls == 0 && rollback_calls == 1 && ready_logs == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_boot_result = ESP_BASE_CONTAINER_BLOCKED;
    assert(!rebooted() && container_boot_calls == 1 && ready_logs == 0);
    esp_base_storage_claim_t blocked_competitor = {0};
    assert(!esp_base_storage_claim(storage_owner, &blocked_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    assert(!rebooted() && ready_logs == 1 && receipt_retire_calls == 1 &&
           container_recover_calls == 1 && receipt_failure_calls == 1);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    interrupted_receipt(true);
    assert(!rebooted() && ready_logs == 1 && receipt_retire_calls == 1 &&
           container_recover_calls == 1 && receipt_failure_calls == 1);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt_retire_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    assert(!rebooted() && ready_logs == 0 && container_boot_calls == 0 &&
           receipt_retire_calls == 1 && container_recover_calls == 0 &&
           receipt_failure_calls == 0 && recovery_logs == 1);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    container_recover_result = ESP_BASE_CONTAINER_RETIRE_UNCERTAIN;
    assert(!rebooted() && ready_logs == 0 && container_boot_calls == 0 &&
           receipt_retire_calls == 1 && container_recover_calls == 1 &&
           receipt_failure_calls == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt_failure_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    assert(!rebooted() && ready_logs == 0 && container_boot_calls == 0 &&
           receipt_retire_calls == 1 && container_recover_calls == 1 &&
           receipt_failure_calls == 1);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(true);
    assert(!rebooted() && ready_logs == 0 && receipt_retire_calls == 0 &&
           container_recover_calls == 0 && receipt_failure_calls == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt_slots.boot_subtype = receipt.target_subtype;
    assert(!rebooted() && ready_logs == 0 && receipt_retire_calls == 0);

    reset_case();
    interrupted_receipt(false);
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_PENDING_VERIFY;
    assert(!rebooted() && ready_logs == 1 && receipt_sha_calls == 1 &&
           receipt_retire_calls == 0 && receipt_failure_calls == 0 &&
           container_selected_calls == 1 && receipt_success_calls == 1);

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    selected_receipt(true, EOTA_STATE_VALID);
    assert(!rebooted() && ready_logs == 1 && container_selected_calls == 1 &&
           container_boot_calls == 1 && receipt_success_calls == 1);

    reset_case();
    container_configured = true;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    selected_receipt(true, EOTA_STATE_VALID);
    receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(!rebooted() && ready_logs == 1 && container_selected_calls == 1 &&
           container_boot_calls == 1 && receipt_success_calls == 0);

    reset_case();
    container_configured = true;
    selected_receipt(true, EOTA_STATE_VALID);
    container_selected_ok = false;
    assert(!rebooted() && ready_logs == 0 && container_selected_calls == 1 &&
           container_boot_calls == 0 && receipt_success_calls == 0);

    reset_case();
    interrupted_receipt(false);
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_PENDING_VERIFY;
    receipt_sha_mismatch = true;
    assert(rebooted() && ready_logs == 0 && recovery_logs == 1 &&
           receipt_sha_calls == 1 && receipt_retire_calls == 0 &&
           container_recover_calls == 0 && receipt_failure_calls == 0 &&
           rollback_calls == 1);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_VALID;
    assert(!rebooted() && ready_logs == 1 && receipt_sha_calls == 1 &&
           receipt_retire_calls == 0 && receipt_failure_calls == 0 &&
           container_selected_calls == 1 && receipt_success_calls == 1);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_VALID;
    assert(!rebooted() && ready_logs == 1 && container_selected_calls == 1 &&
           receipt_success_calls == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_VALID;
    container_selected_ok = false;
    assert(!rebooted() && ready_logs == 0 && container_selected_calls == 1 &&
           receipt_success_calls == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_VALID;
    receipt_success_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    assert(!rebooted() && ready_logs == 0 && container_selected_calls == 1 &&
           receipt_success_calls == 1 && ota_gate_pending);

    reset_case();
    image_state = EOTA_STATE_VALID;
    interrupted_receipt(false);
    receipt.source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    receipt.target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    receipt_slots.running_subtype = receipt_slots.boot_subtype = receipt.target_subtype;
    receipt_slots.target_subtype = receipt.source_subtype;
    receipt_slots.running_state = EOTA_STATE_VALID;
    receipt_sha_mismatch = true;
    assert(!rebooted() && ready_logs == 0 && recovery_logs == 1 &&
           receipt_sha_calls == 1 && receipt_retire_calls == 0 &&
           container_recover_calls == 0 && receipt_failure_calls == 0 &&
           rollback_calls == 0);

#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    puts("  ota_startup_scratch passed (boot recover precedes OTA confirmation; owner released)");
#else
    puts("  ota_startup passed (startup faults, control progress, rollback, readback)");
#endif
}
