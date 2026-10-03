/* Test-only platform wiring for the real app_main -> receipt -> product ledger
 * -> signed Container boot chain. Flash/NVS and firmware observation are fakes;
 * product, slots, signature validation, WAMR and ledger algorithms are real. */
#include <stdarg.h>
#include "esp_app_desc.h"
#include "esp_base_identity.h"
#include "esp_base_protocol.h"
#include "esp_base_safety.h"
#include "esp_base_time.h"
#include "historical_ota_startup_protocol.h"

static void startup_require(bool valid, int line)
{
    if (!valid) {
        fprintf(stderr, "historical_ota_startup: fixture/setup failure line=%d\n", line);
        exit(2);
    }
}
#define STARTUP_REQUIRE(condition) startup_require((condition), __LINE__)
void app_main(void);
static esp_base_ota_receipt_recovery_t startup_receipt;
static esp_base_storage_owner_t *startup_storage_owner;
static eota_flash_io_t startup_flash_io;
static const char startup_new_boot[] = "77777777-7777-4777-8777-777777777777";
static const char *startup_boot = startup_new_boot;
static unsigned startup_ready, startup_recovery_errors, startup_receipt_writes;
static bool startup_gate;

void test_log(const char *format, ...)
{
    char line[320];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (strstr(line, "ESP_BASE_READY")) ++startup_ready;
    if (strstr(line, "RECOVERY_REQUIRED") || strstr(line, "RECOVERY_BLOCKED"))
        ++startup_recovery_errors;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "host"; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t app = {.version = "host"}; return &app; }
const char *esp_get_idf_version(void) { return "host"; }
esp_err_t esp_base_identity_read(esp_base_identity_t *out)
{
    *out = (esp_base_identity_t){.model = "host", .flash_size_bytes = FLASH_BYTES};
    strcpy(out->device_id, "00000000-0000-4000-8000-000000000001");
    return ESP_OK;
}
esp_err_t esp_base_safety_start(esp_base_safety_t *out)
{ *out = (esp_base_safety_t){.reset_reason = "host_reset"}; return ESP_OK; }
esp_err_t esp_base_time_start(const char *server) { (void)server; return ESP_OK; }
eota_policy_t esp_base_ota_policy(bool trusted_time)
{ (void)trusted_time; return (eota_policy_t){0}; }
bool esp_base_ota_policy_bind_flash_io(eota_flash_io_t io)
{ startup_flash_io = io; return io.acquire && io.release && io.context; }
bool eota_available(void) { return true; }
esp_err_t eota_inspect(eota_current_t *out)
{ *out = (eota_current_t){.state = EOTA_STATE_VALID, .running_partition = "ota_1"}; return ESP_OK; }
const char *eota_state_name(eota_state_t state) { (void)state; return "valid"; }
esp_err_t eota_confirm_pending(eota_current_t *out) { (void)out; return ESP_FAIL; }
esp_err_t eota_reject_pending(eota_current_t *out) { (void)out; return ESP_FAIL; }
eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *out)
{
    (void)policy;
    *out = (eota_slots_t){.running_state = EOTA_STATE_VALID,
        .running_subtype = startup_receipt.target_subtype,
        .boot_subtype = startup_receipt.target_subtype,
        .running_address_bytes = 0x200000, .boot_address_bytes = 0x200000};
    return EOTA_UPDATE_OK;
}
eota_result_t eota_sha256_running(const eota_policy_t *policy, uint32_t size,
                                 uint8_t sha[EOTA_SHA256_BYTES])
{ (void)policy; STARTUP_REQUIRE(size == startup_receipt.image_size_bytes);
  memcpy(sha, physical.running_firmware_sha256, 32); return EOTA_UPDATE_OK; }
eota_result_t eota_retire_inactive(const eota_policy_t *policy, uint8_t subtype,
                                  const uint8_t sha[EOTA_SHA256_BYTES])
{ (void)policy; (void)subtype; (void)sha; return EOTA_UPDATE_RESOURCE_FAILURE; }
esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *out)
{ STARTUP_REQUIRE(device_id && out); *out = startup_receipt; return ESP_BASE_OTA_RECEIPT_OK; }
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t result)
{ (void)device_id; (void)operation_id; (void)result; ++startup_receipt_writes;
  return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN; }
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_success(const char *device_id)
{ (void)device_id; ++startup_receipt_writes; return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN; }
esp_err_t esp_base_protocol_load_config(uint32_t *revision,
                                        esp_base_storage_owner_t *owner)
{ STARTUP_REQUIRE(owner == startup_flash_io.context); *revision = 1U; return ESP_OK; }
esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{ startup_storage_owner = context->storage_owner;
  historical_ota_protocol_seed(context, startup_boot); return ESP_OK; }
bool esp_base_protocol_control_healthy(void) { return true; }
uint32_t esp_base_protocol_control_progress_count(void) { return 1U; }
void esp_base_protocol_set_ota_verification_pending(bool pending) { startup_gate = pending; }
bool esp_base_protocol_begin_firmware_package_verification(
    const esp_base_storage_claim_t *claim, const uint8_t sha[32])
{ (void)claim; (void)sha; return false; }
bool esp_base_protocol_firmware_package_health_snapshot(
    esp_base_protocol_firmware_package_health_t *out)
{ (void)out; return false; }
void esp_base_protocol_end_firmware_package_verification(void) {}

static void startup_check(bool passed, const char *what,
                          esp_base_ota_package_mode_t mode, unsigned phase)
{
    if (!passed) {
        fprintf(stderr, "historical_ota_startup: FAIL %s mode=%u phase=%u\n",
                what, (unsigned)mode, phase);
        exit(1); /* Explicit failure: avoid Darwin assertion termination. */
    }
}

static void run_historical_ota_startup(const file_t *key, const file_t *confirmed,
    const file_t *candidate, const char old_boot[37],
    esp_base_ota_package_mode_t mode, unsigned phase, unsigned negative)
{
    configure(key);
    historical_ota_ledger_reset();
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    STARTUP_REQUIRE(esp_base_storage_claim(&owner, &claim));
    STARTUP_REQUIRE(esp_base_container_product_boot(&claim, old_boot) == ESP_BASE_CONTAINER_EMPTY);
    if (mode == ESP_BASE_OTA_PACKAGE_REUSE) {
        install_context_t install = {.package = confirmed, .operation_marker = 0xa1};
        STARTUP_REQUIRE(esp_base_container_with_firmware_set(&claim,
            ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
        STARTUP_REQUIRE(esp_base_container_product_boot(&claim, old_boot) == ESP_BASE_CONTAINER_RUNNING);
    }
    econtainer_slots_state_t state = {0};
    STARTUP_REQUIRE(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t original_sequence = state.sequence;
    startup_receipt = (esp_base_ota_receipt_recovery_t){
        .status = ESP_BASE_OTA_RECEIPT_PREPARED, .container_enabled = true,
        .container_sequence = original_sequence, .package_mode = mode,
        .source_subtype = 0x10, .target_subtype = 0x11, .image_size_bytes = 512U};
    strcpy(startup_receipt.operation_id, "11111111-1111-4111-8111-111111111111");
    memcpy(startup_receipt.source_sha256, physical.running_firmware_sha256, 32);
    memcpy(startup_receipt.inactive_sha256, physical.bootable_firmware_sha256[1], 32);
    memset(startup_receipt.candidate_sha256, 0xc3, 32);
    if (mode != ESP_BASE_OTA_NO_PACKAGE) {
        startup_receipt.package_size_bytes = (uint32_t)confirmed->size;
        startup_receipt.guest_abi_version = 2U;
        startup_receipt.data_schema_version = 1U;
        STARTUP_REQUIRE(SHA256(confirmed->bytes, confirmed->size, startup_receipt.package_sha256));
        memcpy(startup_receipt.trial_event_sha256, trial_event_digest, 32);
        if (mode == ESP_BASE_OTA_PACKAGE_REUSE) {
            startup_receipt.source_package_present = true;
            startup_receipt.source_package_size_bytes = startup_receipt.package_size_bytes;
            startup_receipt.source_guest_abi_version = 2U;
            startup_receipt.source_data_schema_version = 1U;
            memcpy(startup_receipt.source_package_sha256, startup_receipt.package_sha256, 32);
        }
    }
    physical.bootable_count = 1U;
    memset(physical.bootable_firmware_sha256[1], 0, 32);
    STARTUP_REQUIRE(esp_base_container_product_retire_inactive(&claim, &startup_receipt) ==
           ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    if (mode == ESP_BASE_OTA_PACKAGE_REUSE)
        STARTUP_REQUIRE(esp_base_container_product_stop_confirmed(&claim));
    eota_prepared_t prepared = {.image_size_bytes = 512U};
    memcpy(prepared.sha256, startup_receipt.candidate_sha256, 32);
    STARTUP_REQUIRE(esp_base_container_product_stage_firmware(&claim, &prepared, &startup_receipt) ==
           (mode == ESP_BASE_OTA_PACKAGE_WRITE ? ESP_BASE_CONTAINER_STAGE_WRITING :
                                                ESP_BASE_CONTAINER_STAGE_PREPARED));
    if (mode == ESP_BASE_OTA_PACKAGE_WRITE)
        STARTUP_REQUIRE(esp_base_container_product_write_staged_firmware_package(&claim,
            &prepared, &startup_receipt, read_source, (void *)confirmed) == ESP_BASE_CONTAINER_STAGE_PREPARED);
    STARTUP_REQUIRE(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    uint8_t old_boot_bytes[16];
    STARTUP_REQUIRE(decode_uuid(old_boot, old_boot_bytes));
    STARTUP_REQUIRE(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
        prepared.sha256, old_boot_bytes, &state) == ECONTAINER_SLOTS_OK);
    STARTUP_REQUIRE(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
        old_boot_bytes, &state) == ECONTAINER_SLOTS_OK);
    STARTUP_REQUIRE(econtainer_slots_confirm(&io, &geometry, state.sequence,
        prepared.sha256, old_boot_bytes, &state) == ECONTAINER_SLOTS_OK);
    physical.bootable_count = 2U;
    memcpy(physical.running_firmware_sha256, prepared.sha256, 32);
    memcpy(physical.bootable_firmware_sha256[0], prepared.sha256, 32);
    memcpy(physical.bootable_firmware_sha256[1], startup_receipt.source_sha256, 32);
    startup_receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    const econtainer_slots_state_t old_confirmed = state;
    dispose_product();
    configure_product(key);
    STARTUP_REQUIRE(esp_base_container_product_reconcile_selected_ota(&claim,
        &startup_receipt, EOTA_STATE_VALID));
    STARTUP_REQUIRE(esp_base_container_product_boot(&claim, old_boot) ==
        (mode == ESP_BASE_OTA_NO_PACKAGE ? ESP_BASE_CONTAINER_EMPTY : ESP_BASE_CONTAINER_RUNNING));
    STARTUP_REQUIRE(historical_ota_ledger_initialize(&flash_io_owner) == EBASE_LEDGER_OK);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999995",
        .expected_sequence = state.sequence,
        .previous_package_present = mode != ESP_BASE_OTA_NO_PACKAGE,
        .package_size_bytes = (uint32_t)candidate->size,
        .guest_abi_version = 2U, .data_schema_version = 1U};
    if (request.previous_package_present)
        memcpy(request.previous_package_sha256, startup_receipt.package_sha256, 32);
    STARTUP_REQUIRE(SHA256(candidate->bytes, candidate->size, request.package_sha256));
    ebase_product_record_t intent = {.sequence = 1U, .state = EBASE_PRODUCT_PREPARED,
        .container_sequence = request.expected_sequence,
        .kind = request.previous_package_present ? EBASE_PRODUCT_UPGRADE : EBASE_PRODUCT_INSTALL};
    strcpy(intent.operation_id, request.operation_id);
    memset(intent.fingerprint, 0x57, 32);
    memcpy(intent.package_sha256, request.package_sha256, 32);
    if (negative == 1U) intent.operation_id[35] = '6';
    if (negative == 2U) --intent.container_sequence;
    if (negative == 10U) intent.package_sha256[0] ^= 1U;
    STARTUP_REQUIRE(historical_ota_ledger_begin(&flash_io_owner, &intent) == EBASE_LEDGER_OK);
    uint32_t prepared_sequence;
    STARTUP_REQUIRE(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate, &prepared_sequence) == ESP_BASE_CONTAINER_PREPARED);
    if (mode != ESP_BASE_OTA_NO_PACKAGE)
        STARTUP_REQUIRE(esp_base_container_product_stop_confirmed(&claim));
    STARTUP_REQUIRE(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    if (phase >= ECONTAINER_SLOT_TRIAL_STARTED)
        STARTUP_REQUIRE(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
            prepared.sha256, old_boot_bytes, &state) == ECONTAINER_SLOTS_OK);
    if (phase >= ECONTAINER_SLOT_HEALTH_VERIFIED)
        STARTUP_REQUIRE(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
            old_boot_bytes, &state) == ECONTAINER_SLOTS_OK);
    const econtainer_slots_state_t pending = state;
    const size_t candidate_offset = geometry.slots[state.operation.slot].offset_bytes - FLASH_BASE;
    store.flash[candidate_offset] ^= 1U;
    if (negative == 4U) {
        const int current = binding_index(&state, prepared.sha256);
        STARTUP_REQUIRE(current >= 0 && state.bindings[current].package_present);
        store.flash[geometry.slots[state.bindings[current].slot].offset_bytes - FLASH_BASE] ^= 1U;
    }
    if (negative == 5U) startup_receipt.status = ESP_BASE_OTA_RECEIPT_PREPARED;
    if (negative == 6U) physical.bootable_firmware_sha256[1][0] ^= 1U;
    if (negative == 7U) store.fail_flash_read_offset = (uint32_t)candidate_offset + FLASH_BASE;
    if (negative == 8U) store.read_fail_countdown = 1U;
    if (negative == 9U) startup_receipt.candidate_sha256[0] ^= 1U;
    if (negative == 11U) startup_receipt.container_sequence += 50U;
    STARTUP_REQUIRE(esp_base_storage_release(&claim));
    dispose_product();
    configure_product(key);
    startup_boot = negative == 3U ? old_boot : startup_new_boot;
    startup_ready = startup_recovery_errors = startup_receipt_writes = 0U;
    startup_storage_owner = NULL;
    const unsigned blob_writes = store.blob_writes;
    const unsigned ledger_writes = historical_ota_ledger_write_count();
    const unsigned flash_writes = store.flash_writes, erases = store.flash_erases;
    app_main(); /* Real production ordering, real original ledger recovery. */
    ebase_product_ledger_t ledger;
    STARTUP_REQUIRE(historical_ota_ledger_read(startup_flash_io.context, &ledger) == EBASE_LEDGER_OK);
    STARTUP_REQUIRE(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const bool expected_success = negative == 0U;
    startup_check(startup_ready == (expected_success ? 1U : 0U),
                  "startup READY / rejection", mode, phase);
    startup_check(startup_receipt_writes == 0U && startup_receipt.status ==
        (negative == 5U ? ESP_BASE_OTA_RECEIPT_PREPARED : ESP_BASE_OTA_RECEIPT_SUCCEEDED),
        "historical receipt untouched", mode, phase);
    const ebase_product_record_t *record = &ledger.records[ledger.count - 1U];
    startup_check(record->sequence == intent.sequence &&
        !strcmp(record->operation_id, intent.operation_id) &&
        !memcmp(record->fingerprint, intent.fingerprint, 32) &&
        !memcmp(record->package_sha256, intent.package_sha256, 32),
        "original ledger identity", mode, phase);
    startup_check(store.flash_writes == flash_writes && store.flash_erases == erases,
                  "no candidate replay or erase", mode, phase);
    if (expected_success) {
        startup_check(state.phase == ECONTAINER_SLOT_ABORTED &&
            state.sequence == pending.sequence + 1U && record->state == EBASE_PRODUCT_FAILED &&
            record->result_code == 1U && record->container_sequence == state.sequence &&
            store.blob_writes == blob_writes + 1U &&
            historical_ota_ledger_write_count() == ledger_writes + 1U && !startup_gate,
            "exact abandon / ledger FAILED", mode, phase);
        for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index)
            startup_check(same_binding(&state.bindings[index], &old_confirmed.bindings[index]),
                          "confirmed refs unchanged", mode, phase);
        startup_check(atomic_load(&s_product.result) ==
            (mode == ESP_BASE_OTA_NO_PACKAGE ? ESP_BASE_CONTAINER_EMPTY : ESP_BASE_CONTAINER_RUNNING),
            "old confirmed EMPTY or real guest", mode, phase);
        if (mode != ESP_BASE_OTA_NO_PACKAGE) {
            STARTUP_REQUIRE(esp_base_storage_claim(startup_storage_owner, &claim));
            check_active_product(&claim, &expected_confirmed_version, false, NULL);
            STARTUP_REQUIRE(esp_base_container_product_stop_confirmed(&claim));
            STARTUP_REQUIRE(esp_base_storage_release(&claim));
        }
        STARTUP_REQUIRE(esp_base_storage_claim(startup_storage_owner, &claim));
        STARTUP_REQUIRE(esp_base_storage_release(&claim));
        STARTUP_REQUIRE(esp_base_storage_claim(startup_flash_io.context, &claim));
        STARTUP_REQUIRE(esp_base_storage_release(&claim));
    } else {
        startup_check(state.phase == pending.phase && state.sequence == pending.sequence &&
            record->state == EBASE_PRODUCT_PREPARED &&
            record->container_sequence == intent.container_sequence &&
            store.blob_writes == blob_writes && historical_ota_ledger_write_count() == ledger_writes &&
            !s_product.thread_joinable && !atomic_load(&s_product.instance_active),
            "uncertain evidence not abandoned", mode, phase);
    }
    dispose_product();
    STARTUP_REQUIRE(pthread_mutex_destroy(&store.mutex) == 0);
    fprintf(stderr, "historical_ota_startup: PASS mode=%u phase=%u negative=%u\n",
            (unsigned)mode, phase, negative);
}

#undef STARTUP_REQUIRE
