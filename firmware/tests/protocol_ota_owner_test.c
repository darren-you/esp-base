// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the real command branch and its asynchronous completion branch. */
#include "../components/device_protocol/esp_base_protocol.c"

struct esp_base_product_package_source { unsigned marker; };

static esp_base_storage_owner_t owner;
static uint8_t product_bytes[EBASE_PRODUCT_LEDGER_BYTES];
static bool product_present;
static bool pristine_product_baseline;
static unsigned pristine_product_calls;
static esp_base_container_binding_result_t binding_result;
static bool binding_package_present;
static unsigned binding_snapshot_calls;
static esp_base_container_uninstall_result_t product_uninstall_result;
static esp_base_container_boot_result_t product_boot_result;
static unsigned product_uninstall_calls, product_boot_calls;
static esp_base_container_uninstall_recovery_t product_recovery_result;
static unsigned product_recovery_calls;
static esp_base_container_package_recovery_t package_recovery_outcome;
static uint32_t package_recovery_sequence;
static unsigned package_recovery_calls;
static bool package_source_open_ok, package_source_complete_ok;
static unsigned package_source_opens, package_source_closes;
static esp_base_container_prepare_result_t package_prepare_result;
static bool package_prepare_rejected_after_write;
static bool package_prepare_busy_snapshot_uncertain;
static uint32_t binding_sequence;
static bool network_ready, trusted_time_ready;
static esp_base_container_boot_result_t package_trial_result;
static bool package_stop_ok, package_abandon_prepared_ok;
static bool package_event_accepting, package_abandon_trial_ok;
static unsigned package_prepare_calls, package_trial_calls,
    package_stop_calls, package_abandon_prepared_calls,
    package_abandon_trial_calls;
static efrp_aead_flash_store_t frp_store;
static esp_base_ota_receipt_result_t register_result, failure_record_result;
static eota_result_t prepare_result, select_result, retire_result,
    validate_result;
static esp_base_container_stage_result_t stage_result;
static esp_base_container_retire_result_t product_retire_result;
static bool product_configured, product_ota_ready, ota_ready_after_first, snapshot_ok;
static unsigned ota_ready_calls;
static bool worker_created;
static unsigned register_calls, failure_record_calls, task_calls, prepare_calls, stage_calls, select_calls, restart_calls;
static unsigned snapshot_calls, load_receipt_calls, retire_calls,
    product_retire_calls, validate_calls, query_calls;
static char latest_reply[1200];
static char latest_reported[512];
static unsigned reported_calls;
static unsigned wifi_apply_calls, config_commit_calls;
static const char *fake_frp_state = "stopped";
static uint32_t fake_free_heap = 1000;
static esp_base_container_event_observation_result_t fake_event_observation_state;
static int32_t fake_event_guest_result;
static bool fake_event_runtime_ok;
#if defined(CONFIG_IDF_TARGET_ESP32)
static unsigned iram_work_allocations;
#endif

static void reset_case(void)
{
    free(s_reader);
    s_reader = NULL;
    s_serial_discard = false;
    esp_base_storage_owner_init(&owner);
    memset(&s_context, 0, sizeof s_context);
    s_context.device_id = "22222222-2222-4222-8222-222222222222";
    s_context.storage_owner = &owner;
    s_context.flash_io_owner = &owner;
    product_present = false;
    pristine_product_baseline = false;
    pristine_product_calls = 0;
    binding_result = ESP_BASE_CONTAINER_BINDING_OK;
    binding_package_present = false;
    binding_snapshot_calls = 0;
    product_uninstall_result = ESP_BASE_CONTAINER_UNINSTALL_COMPLETE;
    product_boot_result = ESP_BASE_CONTAINER_EMPTY;
    product_uninstall_calls = product_boot_calls = 0;
    product_recovery_result = ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN;
    product_recovery_calls = 0;
    package_recovery_outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN;
    package_recovery_sequence = 0U;
    package_recovery_calls = 0U;
    package_source_open_ok = package_source_complete_ok = true;
    package_source_opens = package_source_closes = 0U;
    package_prepare_result = ESP_BASE_CONTAINER_PREPARED;
    package_prepare_rejected_after_write = false;
    package_prepare_busy_snapshot_uncertain = false;
    binding_sequence = 6U;
    network_ready = trusted_time_ready = true;
    package_trial_result = ESP_BASE_CONTAINER_RUNNING;
    package_stop_ok = package_abandon_prepared_ok = true;
    package_event_accepting = true;
    package_abandon_trial_ok = false;
    package_prepare_calls = package_trial_calls = package_stop_calls =
        package_abandon_prepared_calls = package_abandon_trial_calls = 0U;
    memset(product_bytes, 0, sizeof product_bytes);
    strcpy(s_boot_id, "33333333-3333-4333-8333-333333333333");
    memset(&s_guard, 0, sizeof s_guard);
    memset(s_outcomes, 0, sizeof s_outcomes);
    memset(s_frp_status_seen, 0, sizeof s_frp_status_seen);
    memset(&s_ota_storage_claim, 0, sizeof s_ota_storage_claim);
    free(s_product_request);
    s_product_request = NULL;
    memset(&s_product_storage_claim, 0, sizeof s_product_storage_claim);
    memset(s_product_operation_id, 0, sizeof s_product_operation_id);
    memset(s_product_fingerprint, 0, sizeof s_product_fingerprint);
    s_product_operation_sequence = s_product_trial_sequence = 0U;
    s_product_active = s_product_trial_running = false;
    atomic_store(&s_product_done, false);
    atomic_store(&s_product_result, PRODUCT_WORK_UNCERTAIN);
    atomic_store(&s_product_resolved_sequence, 0U);
    memset(&s_ota_request, 0, sizeof s_ota_request);
    s_config_uncertain = s_ota_boot_uncertain = s_ota_active = s_trial_active = false;
    atomic_store(&s_ota_done, false);
    atomic_store(&s_ota_stage_uncertain, false);
    atomic_store(&s_ota_received, 0);
    esp_base_control_state_set_ota_pending(&s_control_state, false);
    esp_base_control_state_set_ota_download_active(&s_control_state, false);
    register_result = failure_record_result = ESP_BASE_OTA_RECEIPT_OK;
    prepare_result = select_result = retire_result = validate_result = EOTA_UPDATE_OK;
    stage_result = ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED;
    product_retire_result = ESP_BASE_CONTAINER_RETIRE_COMPLETE;
    product_configured = false;
    product_ota_ready = true;
    snapshot_ok = true;
    ota_ready_after_first = true;
    ota_ready_calls = 0;
    worker_created = true;
    register_calls = failure_record_calls = task_calls = prepare_calls = stage_calls = select_calls = restart_calls = 0;
    snapshot_calls = load_receipt_calls = retire_calls = product_retire_calls = 0;
    validate_calls = query_calls = 0;
    latest_reply[0] = '\0';
    latest_reported[0] = '\0';
    reported_calls = 0;
    fake_frp_state = "stopped";
    fake_free_heap = 1000;
    fake_event_observation_state = ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION;
    fake_event_guest_result = 3;
    fake_event_runtime_ok = true;
#if defined(CONFIG_IDF_TARGET_ESP32)
    iram_work_allocations = 0;
#endif
    wifi_apply_calls = config_commit_calls = 0;
}

static void start(unsigned request_number)
{
    char line[32];
    const int length = snprintf(line, sizeof line, "start-%u", request_number);
    assert(length > 0 && (size_t)length < sizeof line);
    s_reply_mqtt = true;
    handle_line(line, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void ota_result(unsigned operation_number)
{
    char line[32];
    const int length = snprintf(line, sizeof line, "result-%u", operation_number);
    assert(length > 0 && (size_t)length < sizeof line);
    s_reply_mqtt = true;
    handle_line(line, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void product_uninstall(unsigned request_number)
{
    char line[32];
    const int length = snprintf(line, sizeof line, "uninstall-%u", request_number);
    assert(length > 0 && (size_t)length < sizeof line);
    s_reply_mqtt = true;
    handle_line(line, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void product_package(unsigned request_number)
{
    char line[40];
    const int length = snprintf(line, sizeof line, "package-%u", request_number);
    assert(length > 0 && (size_t)length < sizeof line);
    s_reply_mqtt = true;
    handle_line(line, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void config_set(unsigned request_number, bool via_mqtt, char *usb_reply,
                       size_t usb_reply_capacity)
{
    char line[32];
    const int length = snprintf(line, sizeof line, "config-%u", request_number);
    assert(length > 0 && (size_t)length < sizeof line);
    if (via_mqtt) {
        s_reply_mqtt = true;
        handle_line(line, (size_t)length, NULL);
        s_reply_mqtt = false;
        return;
    }
    assert(usb_reply != NULL && usb_reply_capacity > 0U);
    fflush(stdout);
    FILE *capture = tmpfile();
    assert(capture != NULL);
    const int original = dup(fileno(stdout));
    assert(original >= 0 && dup2(fileno(capture), fileno(stdout)) >= 0);
    handle_line(line, (size_t)length, NULL);
    fflush(stdout);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    const size_t got = fread(usb_reply, 1, usb_reply_capacity - 1U, capture);
    usb_reply[got] = '\0';
    assert(dup2(original, fileno(stdout)) >= 0);
    close(original);
    fclose(capture);
}

static void expect_reply(const char *state, const char *error)
{
    char field[100];
    (void)snprintf(field, sizeof field, "\"state\":\"%s\"", state);
    if (!strstr(latest_reply, field))
        fprintf(stderr, "expected %s in %s\n", field, latest_reply);
    assert(strstr(latest_reply, field));
    if (error) {
        (void)snprintf(field, sizeof field, "\"error_code\":\"%s\"", error);
        if (!strstr(latest_reply, field))
            fprintf(stderr, "expected %s in %s\n", field, latest_reply);
        assert(strstr(latest_reply, field));
    }
}

static void initialize_empty_product_ledger(void)
{
    ebase_product_ledger_t ledger = {0};
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_UNINITIALIZED);
    assert(ebase_product_ledger_initialize_empty(&ledger, &io) == EBASE_LEDGER_OK);
}

static void check_product_uninstall_path(void)
{
    reset_case();
    product_configured = true;
    binding_package_present = true;
    initialize_empty_product_ledger();
    product_uninstall(31U);
    expect_reply("succeeded", NULL);
    assert(product_uninstall_calls == 1U && product_boot_calls == 1U &&
           atomic_load(&owner.active_token) == 0U);
    ebase_product_ledger_t ledger = {0};
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.count == 1U && ledger.records[0].kind == EBASE_PRODUCT_UNINSTALL &&
           ledger.records[0].state == EBASE_PRODUCT_SUCCEEDED &&
           ledger.records[0].container_sequence == 7U);
    product_uninstall(32U); /* A new request ID cannot re-execute the same operation ID. */
    expect_reply("succeeded", NULL);
    assert(product_uninstall_calls == 1U && product_boot_calls == 1U);
    product_uninstall(34U); /* Same operation ID, different persisted fingerprint. */
    expect_reply("failed", "product_operation_conflict");
    assert(product_uninstall_calls == 1U && atomic_load(&owner.active_token) == 0U);

    reset_case();
    product_configured = true;
    binding_package_present = true;
    initialize_empty_product_ledger();
    product_uninstall_result = ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN;
    product_uninstall(33U);
    expect_reply("unknown", "storage_uncertain");
    assert(product_uninstall_calls == 1U && s_config_uncertain &&
           atomic_load(&owner.active_token) != 0U);
    uint8_t persisted[EBASE_PRODUCT_LEDGER_BYTES];
    memcpy(persisted, product_bytes, sizeof persisted);
    reset_case(); /* Fresh boot: retain NVS, discard the old in-memory claim. */
    memcpy(product_bytes, persisted, sizeof persisted);
    product_present = true;
    product_configured = true;
    product_recovery_result = ESP_BASE_CONTAINER_UNINSTALL_RECOVERED;
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_protocol_prepare_product_ledger(&claim, true));
    assert(product_recovery_calls == 1U && esp_base_storage_release(&claim));
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_SUCCEEDED &&
           ledger.records[0].container_sequence == 7U);

    reset_case();
    product_configured = true;
    binding_package_present = true;
    initialize_empty_product_ledger();
    product_uninstall_result = ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN;
    product_uninstall(33U);
    memcpy(persisted, product_bytes, sizeof persisted);
    reset_case();
    memcpy(product_bytes, persisted, sizeof persisted);
    product_present = true;
    product_configured = true;
    product_recovery_result = ESP_BASE_CONTAINER_UNINSTALL_NOT_COMMITTED;
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_protocol_prepare_product_ledger(&claim, false));
    assert(product_recovery_calls == 1U && esp_base_storage_release(&claim));
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].result_code == 1U &&
           ledger.records[0].container_sequence == 6U);
}

static void check_product_package_guard(void)
{
    reset_case();
    product_package(40U);
    expect_reply("failed", "product_not_configured");
    assert(restart_calls == 0U && task_calls == 0U &&
           atomic_load(&owner.active_token) == 0U);
    product_package(41U); /* Same request ID, different signed package URL. */
    expect_reply("failed", "request_conflict");
    assert(restart_calls == 0U && task_calls == 0U &&
           atomic_load(&owner.active_token) == 0U);
    product_package(50U);
    expect_reply("failed", "product_not_configured");
    assert(restart_calls == 0U && task_calls == 0U &&
           atomic_load(&owner.active_token) == 0U);
    product_package(42U); /* URL preflight runs before request admission. */
    expect_reply("failed", "invalid_request");
    assert(restart_calls == 0U && task_calls == 0U &&
           atomic_load(&owner.active_token) == 0U);
    assert(!product_present && !binding_snapshot_calls && !product_uninstall_calls);
    const uint8_t empty[EBASE_PRODUCT_LEDGER_BYTES] = {0};
    assert(memcmp(product_bytes, empty, sizeof empty) == 0);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    product_package(40U);
    expect_reply("running", NULL);
    assert(task_calls == 1U && package_source_opens == 1U &&
           package_source_closes == 1U && package_prepare_calls == 1U &&
           package_trial_calls == 1U && package_stop_calls == 0U);
    poll_product();
    expect_reply("running", NULL);
    assert(s_product_active && s_product_trial_running &&
           s_product_trial_sequence == 9U &&
           atomic_load(&owner.active_token) == 0U);
    ebase_product_ledger_t ledger = {0};
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.count == 1U &&
           ledger.records[0].state == EBASE_PRODUCT_PREPARED &&
           ledger.records[0].kind == EBASE_PRODUCT_INSTALL);
    s_reply_mqtt = true;
    handle_line("product-status-45", sizeof("product-status-45") - 1U, NULL);
    s_reply_mqtt = false;
    expect_reply("succeeded", NULL);
    assert(strstr(latest_reply, "\"pending_operation_id\":\"44444444-4444-4444-8444-000000000040\"") &&
           strstr(latest_reply, "\"container_sequence\":9"));
    product_package(40U);
    expect_reply("running", NULL);
    product_package(41U);
    expect_reply("failed", "request_conflict");
    product_package(50U);
    expect_reply("failed", "product_operation_conflict");
    product_package(43U); /* Same operation, new request ID, changed URL. */
    expect_reply("failed", "product_operation_conflict");
    assert(package_prepare_calls == 1U && task_calls == 1U);
    package_event_accepting = false; /* Candidate runtime stopped unexpectedly. */
    package_abandon_trial_ok = true;
    poll_product_trial_failure();
    expect_reply("failed", "product_runtime_failed");
    assert(package_abandon_trial_calls == 1U && product_boot_calls == 1U &&
           !s_product_active && !s_product_trial_running &&
           atomic_load(&owner.active_token) == 0U);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 10U);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    package_source_open_ok = false;
    product_package(40U);
    expect_reply("running", NULL);
    poll_product();
    expect_reply("failed", "product_operation_failed");
    assert(package_prepare_calls == 0U &&
           atomic_load(&owner.active_token) == 0U && !s_product_active);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 6U);
    network_ready = trusted_time_ready = false;
    product_package(44U); /* Ledger lookup precedes new-transfer admission. */
    expect_reply("failed", "product_operation_failed");
    assert(package_source_opens == 1U && task_calls == 1U);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    package_prepare_result = ESP_BASE_CONTAINER_PREPARE_BUSY;
    product_package(40U);
    expect_reply("running", NULL);
    poll_product();
    expect_reply("failed", "product_operation_failed");
    assert(package_prepare_calls == 1U && binding_snapshot_calls == 2U &&
           atomic_load(&owner.active_token) == 0U && !s_product_active);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 6U);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    package_prepare_result = ESP_BASE_CONTAINER_PREPARE_BUSY;
    package_prepare_busy_snapshot_uncertain = true;
    product_package(40U);
    expect_reply("running", NULL);
    poll_product();
    expect_reply("unknown", "storage_uncertain");
    assert(package_prepare_calls == 1U && binding_snapshot_calls == 2U &&
           atomic_load(&owner.active_token) != 0U && s_product_active &&
           s_config_uncertain);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_PREPARED);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    package_prepare_result = ESP_BASE_CONTAINER_PREPARE_REJECTED;
    package_prepare_rejected_after_write = true;
    product_package(40U);
    expect_reply("running", NULL);
    poll_product();
    expect_reply("failed", "product_operation_failed");
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 9U &&
           atomic_load(&owner.active_token) == 0U);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    package_source_complete_ok = false;
    product_package(40U);
    expect_reply("running", NULL);
    poll_product();
    expect_reply("failed", "product_operation_failed");
    assert(package_abandon_prepared_calls == 1U &&
           atomic_load(&owner.active_token) == 0U);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 9U);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    worker_created = false;
    product_package(40U);
    expect_reply("failed", "resource_failure");
    assert(task_calls == 1U && package_source_opens == 0U &&
           atomic_load(&owner.active_token) == 0U);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 6U);
}

bool esp_base_product_package_source_request_valid(
    const char *url, uint32_t expected_size_bytes)
{
    return url != NULL && expected_size_bytes != 0U &&
        (strcmp(url, "https://packages.example.test/a.pkg") == 0 ||
         strcmp(url, "https://packages.example.test/b.pkg") == 0);
}

esp_base_product_package_source_t *esp_base_product_package_source_open(
    const char *url, uint32_t expected_size_bytes, bool trusted_time)
{
    assert(trusted_time && esp_base_product_package_source_request_valid(
        url, expected_size_bytes));
    ++package_source_opens;
    static esp_base_product_package_source_t source;
    return package_source_open_ok ? &source : NULL;
}

bool esp_base_product_package_source_read(void *context, size_t offset_bytes,
    uint8_t *destination, size_t size_bytes)
{
    assert(context != NULL && offset_bytes == 0U && destination != NULL &&
           size_bytes == 1U);
    *destination = 0x7b;
    return true;
}

bool esp_base_product_package_source_complete(
    const esp_base_product_package_source_t *source)
{
    assert(source != NULL);
    return package_source_complete_ok;
}

void esp_base_product_package_source_close(
    esp_base_product_package_source_t *source)
{
    assert(source != NULL);
    ++package_source_closes;
}

static void check_product_package_preboot_recovery(void)
{
    reset_case();
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_protocol_recover_product_package(&claim));
    assert(package_recovery_calls == 0U && !product_present);
    assert(esp_base_storage_release(&claim));

    reset_case();
    initialize_empty_product_ledger();
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    ebase_product_ledger_t ledger = {0};
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    ebase_product_record_t intent = {.sequence = 1U, .container_sequence = 6U,
        .kind = EBASE_PRODUCT_UPGRADE, .state = EBASE_PRODUCT_PREPARED};
    strcpy(intent.operation_id, "44444444-4444-4444-8444-000000000010");
    memset(intent.fingerprint, 0x5a, sizeof intent.fingerprint);
    memset(intent.package_sha256, 0xab, sizeof intent.package_sha256);
    assert(ebase_product_ledger_begin(&ledger, &io, &intent) == EBASE_LEDGER_OK);
    assert(esp_base_storage_claim(&owner, &claim));
    assert(!esp_base_protocol_recover_product_package(&claim));
    assert(package_recovery_calls == 1U && esp_base_storage_claim_active(&claim));
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_PREPARED);
    package_recovery_outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED;
    package_recovery_sequence = 9U;
    assert(esp_base_protocol_recover_product_package(&claim));
    assert(package_recovery_calls == 2U &&
           ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_FAILED &&
           ledger.records[0].container_sequence == 9U);
    assert(esp_base_storage_release(&claim));

    reset_case();
    initialize_empty_product_ledger();
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    assert(ebase_product_ledger_begin(&ledger, &io, &intent) == EBASE_LEDGER_OK);
    assert(esp_base_storage_claim(&owner, &claim));
    package_recovery_outcome = ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED;
    package_recovery_sequence = 11U;
    assert(esp_base_protocol_recover_product_package(&claim));
    assert(package_recovery_calls == 1U &&
           ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_SUCCEEDED &&
           ledger.records[0].result_code == 0U &&
           ledger.records[0].container_sequence == 11U);
    assert(esp_base_protocol_recover_product_package(&claim) &&
           package_recovery_calls == 1U);
    assert(esp_base_storage_release(&claim));
}

static void check_frp_status(const char *request, int expected_http,
                             const char *expected_error)
{
    char response[1024] = {0};
    size_t length = 0;
    const int http = handle_frp_status((const uint8_t *)request, strlen(request),
                                       response, sizeof response, &length, NULL);
    assert(http == expected_http && length > 0 && length < sizeof response);
    if (expected_error) assert(strstr(response, expected_error));
    else {
        char heap_field[48];
        snprintf(heap_field, sizeof heap_field, "\"free_heap\":%u", !strcmp(request, "status-1") ? 1000u : fake_free_heap);
        assert(strstr(response, "\"state\":\"succeeded\"") &&
               strstr(response, "\"frp\":\"stopped\"") && strstr(response, heap_field));
    }
}

int main(void)
{
    reset_case();
    s_reply_mqtt = true;
    handle_line("product-status-1", strlen("product-status-1"), NULL);
    s_reply_mqtt = false;
    expect_reply("unknown", "product_ledger_uninitialized");
    assert(binding_snapshot_calls == 0U);
    s_reply_mqtt = true;
    handle_line("product-1", 9, NULL);
    s_reply_mqtt = false;
    expect_reply("unknown", "product_operation_not_found");
    esp_base_storage_owner_t product_owner = {0};
    esp_base_storage_owner_init(&product_owner);
    esp_base_storage_claim_t product_claim = {0};
    assert(esp_base_storage_claim(&product_owner, &product_claim));
    assert(!esp_base_protocol_prepare_product_ledger(&product_claim, true));
    assert(!product_present && pristine_product_calls == 1U);
    pristine_product_baseline = true;
    assert(esp_base_protocol_prepare_product_ledger(&product_claim, true));
    assert(product_present && pristine_product_calls == 2U);
    pristine_product_baseline = false;
    assert(esp_base_protocol_prepare_product_ledger(&product_claim, true));
    assert(pristine_product_calls == 2U);
    assert(esp_base_storage_release(&product_claim));
    const ebase_product_ledger_io_t product_io = ebase_product_ledger_nvs_io(&owner);
    ebase_product_ledger_t product_ledger;
    assert(ebase_product_ledger_open(&product_ledger, &product_io) == EBASE_LEDGER_OK);
    assert(product_ledger.high_watermark == 0U);
    ebase_product_record_t product_intent = {.sequence = 1U,
        .container_sequence = 6U, .kind = EBASE_PRODUCT_INSTALL,
        .state = EBASE_PRODUCT_PREPARED};
    strcpy(product_intent.operation_id, "44444444-4444-4444-8444-000000000001");
    memset(product_intent.fingerprint, 0x5a, sizeof product_intent.fingerprint);
    memset(product_intent.package_sha256, 0xab, sizeof product_intent.package_sha256);
    assert(ebase_product_ledger_begin(&product_ledger, &product_io, &product_intent) == EBASE_LEDGER_OK);
    assert(ebase_product_ledger_finish(&product_ledger, &product_io, 1U,
        product_intent.operation_id, product_intent.fingerprint,
        EBASE_PRODUCT_SUCCEEDED, 0U, 10U) == EBASE_LEDGER_OK);
    s_reply_mqtt = true;
    handle_line("product-1", 9, NULL);
    s_reply_mqtt = false;
    expect_reply("succeeded", NULL);
    assert(strstr(latest_reply, "\"operation_sequence\":1") &&
           strstr(latest_reply, "\"container_sequence\":10") &&
           strstr(latest_reply, "\"package_sha256\":\"abab"));
    s_reply_mqtt = true;
    handle_line("product-status-1", strlen("product-status-1"), NULL);
    s_reply_mqtt = false;
    expect_reply("succeeded", NULL);
    assert(strstr(latest_reply, "\"operation_sequence_high_watermark\":1") &&
           strstr(latest_reply, "\"next_operation_sequence\":2") &&
           strstr(latest_reply, "\"pending_operation_id\":null") &&
           strstr(latest_reply, "\"container_sequence\":6") &&
           strstr(latest_reply, "\"package_sha256\":null") &&
           binding_snapshot_calls == 1U);
    product_intent.sequence = 2U;
    strcpy(product_intent.operation_id, "44444444-4444-4444-8444-000000000002");
    assert(ebase_product_ledger_begin(&product_ledger, &product_io, &product_intent) == EBASE_LEDGER_OK);
    s_reply_mqtt = true;
    handle_line("product-status-1", strlen("product-status-1"), NULL);
    s_reply_mqtt = false;
    expect_reply("succeeded", NULL);
    assert(strstr(latest_reply, "\"next_operation_sequence\":3") &&
           strstr(latest_reply, "\"pending_operation_id\":\"44444444-4444-4444-8444-000000000002\""));
    binding_package_present = true;
    s_reply_mqtt = true;
    handle_line("product-status-1", strlen("product-status-1"), NULL);
    s_reply_mqtt = false;
    assert(strstr(latest_reply, "\"package_sha256\":\"7b7b") &&
           atomic_load(&owner.active_token) == 0U);
    binding_result = ESP_BASE_CONTAINER_BINDING_UNCERTAIN;
    s_reply_mqtt = true;
    handle_line("product-status-1", strlen("product-status-1"), NULL);
    s_reply_mqtt = false;
    expect_reply("unknown", "storage_uncertain");
    assert(s_config_uncertain && atomic_load(&owner.active_token) != 0U);
    s_reply_mqtt = true;
    reply("11111111-1111-4111-8111-111111111111", "succeeded", NULL, NULL);
    s_reply_mqtt = false;
    char copied_result[sizeof latest_reply];
    strcpy(copied_result, latest_reply);
    fake_event_observation_state = ESP_BASE_CONTAINER_EVENT_OBSERVED;
    reported();
    assert(reported_calls == 1 && strstr(latest_reported, "\"frp_state\":\"stopped\"") &&
           strstr(latest_reported, "\"last_accepted_event_sequence\":3") &&
           strstr(latest_reported, "\"last_completed_event_sequence\":2") &&
           strstr(latest_reported, "\"last_completed_package_sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\"") &&
           strstr(latest_reported, "\"last_event_outcome\":\"succeeded\"") &&
           strstr(latest_reported, "\"last_guest_result\":3") &&
           !strcmp(latest_reply, copied_result));
    char copied_reported[sizeof latest_reported];
    strcpy(copied_reported, latest_reported);
    s_reply_mqtt = true;
    reply("11111111-1111-4111-8111-111111111112", "failed", "invalid_request", NULL);
    s_reply_mqtt = false;
    assert(!strcmp(latest_reported, copied_reported));
    fake_event_guest_result = -7;
    reported();
    assert(reported_calls == 2 &&
           strstr(latest_reported, "\"last_event_outcome\":\"business_failed\"") &&
           strstr(latest_reported, "\"last_guest_result\":-7"));
    fake_event_runtime_ok = false;
    reported();
    assert(reported_calls == 3 &&
           strstr(latest_reported, "\"last_event_outcome\":\"runtime_failed\"") &&
           strstr(latest_reported, "\"last_guest_result\":null"));
    strcpy(copied_reported, latest_reported);
    char oversized_state[512];
    memset(oversized_state, 'x', sizeof oversized_state - 1);
    oversized_state[sizeof oversized_state - 1] = '\0';
    fake_frp_state = oversized_state;
    reported();
    assert(reported_calls == 3 && !strcmp(latest_reported, copied_reported));

    reset_case();
    s_context.config.revision = 7U;
    s_context.config.frp.configured = true;
    strcpy(s_context.config.frp.server_hostname, "old-frp.example");
    char usb_reply[1200];
    config_set(20U, false, usb_reply, sizeof usb_reply);
    assert(strstr(usb_reply, "\"state\":\"failed\"") &&
           strstr(usb_reply, "\"error_code\":\"frp_storage_unavailable\""));
    assert(s_context.config.revision == 7U && s_context.config.frp.configured &&
           !strcmp(s_context.config.frp.server_hostname, "old-frp.example") &&
           !s_trial_active && s_candidate == NULL &&
           atomic_load(&owner.active_token) == 0U);

    reset_case();
    s_context.config.revision = 7U;
    feed_serial((const unsigned char *)"config-20", 9U);
    assert(s_reader != NULL && s_reader->length == 9U && !s_serial_discard);
    feed_serial((const unsigned char *)"\n", 1U);
    assert(s_reader == NULL && !s_serial_discard && !s_trial_active);
    unsigned char invalid[EBASE_LINE_LIMIT + 1U];
    memset(invalid, 'x', sizeof invalid);
    feed_serial(invalid, sizeof invalid);
    assert(s_reader == NULL && s_serial_discard);
    feed_serial((const unsigned char *)"tail\nconfig-24\n", 15U);
    assert(s_reader == NULL && !s_serial_discard && !s_trial_active);

    reset_case();
    s_context.config.revision = 7U;
    s_context.frp_flash_store = &frp_store;
    config_set(23U, false, usb_reply, sizeof usb_reply);
    assert(strstr(usb_reply, "\"state\":\"running\"") && s_trial_active &&
           s_candidate != NULL && s_candidate->revision == 7U && wifi_apply_calls == 1U);
    poll_configuration(1001U);
    assert(!s_trial_active && s_candidate == NULL && config_commit_calls == 1U &&
           s_context.config.revision == 8U && wifi_apply_calls == 1U);
#if defined(CONFIG_IDF_TARGET_ESP32)
    assert(iram_work_allocations == 3U);
#endif

    reset_case();
    s_context.config.revision = 7U;
    s_context.config.frp.configured = true;
    strcpy(s_context.config.frp.server_hostname, "old-frp.example");
    config_set(21U, true, NULL, 0U);
    expect_reply("failed", "physical_usb_required");
    assert(s_context.config.revision == 7U && s_context.config.frp.configured &&
           !strcmp(s_context.config.frp.server_hostname, "old-frp.example") &&
           !s_trial_active && s_candidate == NULL &&
           atomic_load(&owner.active_token) == 0U);

    reset_case();
    check_frp_status("status-1", 200, NULL);
    fake_free_heap = 500;
    check_frp_status("status-1", 200, NULL);
    check_frp_status("changed-deadline", 409, "request_conflict");
    check_frp_status("changed-boot", 409, "request_conflict");
    check_frp_status("status-2", 200, NULL);
    check_frp_status("wrong-boot", 409, "wrong_boot");
    check_frp_status("wrong-device", 409, "wrong_device");
    check_frp_status("expired", 409, "expired");
    check_frp_status("far", 400, "invalid_deadline");
    check_frp_status("invalid", 400, "invalid_request");
    reset_case();
    validate_result = EOTA_UPDATE_INVALID_REQUEST;
    start(19);
    expect_reply("failed", "invalid_request");
    assert(validate_calls == 1 && register_calls == 0 && snapshot_calls == 0 &&
           retire_calls == 0 && task_calls == 0 &&
           atomic_load(&owner.active_token) == 0);

    reset_case();
    esp_base_storage_claim_t other = {0};
    assert(esp_base_storage_claim(&owner, &other));
    start(1);
    expect_reply("failed", "operation_busy");
    assert(register_calls == 0 && task_calls == 0);
    assert(atomic_load(&owner.next_token) == other.token);
    assert(esp_base_storage_release(&other));

    reset_case();
    product_ota_ready = false;
    start(13);
    expect_reply("failed", "product_ota_unavailable");
    assert(register_calls == 0 && prepare_calls == 0 && stage_calls == 0);
    assert(atomic_load(&owner.active_token) == 0);

    reset_case();
    ota_ready_after_first = false;
    start(14);
    poll_ota();
    expect_reply("failed", "resource_failure");
    assert(ota_ready_calls == 2 && register_calls == 1 &&
           prepare_calls == 0 && stage_calls == 0 && select_calls == 0 &&
           atomic_load(&owner.active_token) == 0);

    reset_case();
    register_result = ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    start(2);
    expect_reply("failed", "ota_slot_unavailable");
    assert(register_calls == 1 && task_calls == 0);
    assert(atomic_load(&owner.active_token) == 0);

    reset_case();
    register_result = ESP_BASE_OTA_RECEIPT_SAME_IMAGE;
    start(22);
    expect_reply("failed", "ota_same_image");
    assert(snapshot_calls == 1 && register_calls == 1 &&
           task_calls == 0 && retire_calls == 0 && product_retire_calls == 0 &&
           prepare_calls == 0 && !s_ota_active &&
           atomic_load(&owner.active_token) == 0);
    start(22); /* The same request replays its failed result without a worker. */
    expect_reply("failed", "ota_same_image");
    assert(register_calls == 1 && task_calls == 0);
    ota_result(22);
    expect_reply("unknown", "ota_operation_not_found");
    assert(query_calls == 1 && register_calls == 1 && task_calls == 0);

    reset_case();
    register_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    start(3);
    expect_reply("unknown", "storage_uncertain");
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    assert(s_config_uncertain);

    reset_case();
    worker_created = false;
    start(4);
    expect_reply("failed", "resource_failure");
    assert(register_calls == 1 && failure_record_calls == 1 && task_calls == 1);
    assert(atomic_load(&owner.active_token) == 0);

    reset_case();
    worker_created = false;
    failure_record_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    start(5);
    expect_reply("unknown", "storage_uncertain");
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    prepare_result = EOTA_UPDATE_DOWNLOAD_FAILED;
    start(6);
    expect_reply("running", NULL);
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    product_configured = true;
    prepare_result = EOTA_UPDATE_DOWNLOAD_FAILED;
    start(15);
    assert(prepare_calls == 1 && stage_calls == 0 && select_calls == 0);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    product_configured = true;
    stage_result = ESP_BASE_CONTAINER_STAGE_PREPARED;
    start(9);
    assert(stage_calls == 1 && select_calls == 1);
    poll_ota();
    assert(restart_calls == 1 && failure_record_calls == 0);

    reset_case();
    product_configured = true;
    stage_result = ESP_BASE_CONTAINER_STAGE_REJECTED;
    start(10);
    assert(stage_calls == 1 && select_calls == 0);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && s_config_uncertain &&
           atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    product_configured = true;
    stage_result = ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
    start(11);
    assert(stage_calls == 1 && select_calls == 0);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && s_config_uncertain && s_ota_boot_uncertain &&
           atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    product_configured = true;
    stage_result = ESP_BASE_CONTAINER_STAGE_PREPARED;
    select_result = EOTA_UPDATE_RESOURCE_FAILURE;
    start(12);
    assert(stage_calls == 1 && select_calls == 1);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    select_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    start(8);
    assert(prepare_calls == 1 && select_calls == 1);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(s_ota_boot_uncertain && failure_record_calls == 0);
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    start(7);
    expect_reply("running", NULL);
    assert(prepare_calls == 1 && select_calls == 1);
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    poll_ota();
    assert(restart_calls == 1 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    start(7); /* Admission replay must not register or download again. */
    assert(register_calls == 1 && task_calls == 1);
    assert(snapshot_calls == 1 && load_receipt_calls == 1 &&
           retire_calls == 1 && product_retire_calls == 1);
    reset_case();
    snapshot_ok = false;
    start(16);
    expect_reply("failed", "product_ota_unavailable");
    assert(snapshot_calls == 1 && register_calls == 0 &&
           retire_calls == 0 && product_retire_calls == 0 && task_calls == 0 &&
           atomic_load(&owner.active_token) == 0);

    reset_case();
    retire_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    start(17);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(retire_calls == 1 && product_retire_calls == 0 && prepare_calls == 0 &&
           atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    product_retire_result = ESP_BASE_CONTAINER_RETIRE_UNCERTAIN;
    start(18);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(retire_calls == 1 && product_retire_calls == 1 && prepare_calls == 0);
    check_product_uninstall_path();
    check_product_package_guard();
    check_product_package_preboot_recovery();
    puts("  protocol_ota_owner passed (OTA owner faults; product ledger/uninstall/recovery; USB FRP storage gate; MQTT write rejection)");
}

const char *ebase_parse_command(const char *line, size_t length, ebase_command_t *out)
{
    if (line == NULL) {
        memset(out, 0, sizeof *out);
        return "invalid_request";
    }
    unsigned number = 0;
    const bool configure = length > 7U && sscanf(line, "config-%u", &number) == 1;
    const bool query = length > 7U && sscanf(line, "result-%u", &number) == 1;
    const bool product_query = length > 8U && sscanf(line, "product-%u", &number) == 1;
    const bool product_status_query = length > 15U && sscanf(line, "product-status-%u", &number) == 1;
    const bool product_uninstall_command =
        length > 10U && sscanf(line, "uninstall-%u", &number) == 1;
    const bool product_package_command =
        length > 8U && sscanf(line, "package-%u", &number) == 1;
    assert(configure || query || product_query || product_status_query ||
           product_uninstall_command || product_package_command ||
           (length > 6U && sscanf(line, "start-%u", &number) == 1));
    memset(out, 0, sizeof *out);
    out->kind = configure ? EBASE_CONFIG_SET :
                query ? EBASE_OTA_RESULT :
                product_query ? EBASE_PRODUCT_RESULT :
                product_status_query ? EBASE_PRODUCT_STATUS :
                product_package_command ? (number < 50U ?
                    EBASE_PRODUCT_INSTALL_COMMAND : EBASE_PRODUCT_UPGRADE_COMMAND) :
                product_uninstall_command ? EBASE_PRODUCT_UNINSTALL_COMMAND : EBASE_OTA_START;
    snprintf(out->request.request_id, sizeof out->request.request_id,
             "11111111-1111-4111-8111-%012u",
             product_package_command && number == 41U ? 40U : number);
    strcpy(out->request.device_id, "22222222-2222-4222-8222-222222222222");
    strcpy(out->request.boot_id, "33333333-3333-4333-8333-333333333333");
    out->request.expires_at_ms = 10000;
    if (configure) {
        out->config.revision = 7U;
        out->config.frp.configured = true;
        out->config.wifi.configured = true;
        return NULL;
    }
    if (query || product_query) {
        snprintf(out->operation_id, sizeof out->operation_id,
                 "44444444-4444-4444-8444-%012u", number);
        return NULL;
    }
    if (product_uninstall_command) {
        strcpy(out->product_uninstall.operation_id,
               "44444444-4444-4444-8444-000000000001");
        out->product_uninstall.operation_sequence = number == 34U ? 2U : 1U;
        out->product_uninstall.expected_container_sequence = 6U;
        memset(out->product_uninstall.package_sha256, 0x7b, 32);
        return NULL;
    }
    if (product_package_command) {
        ebase_product_package_request_t *package = &out->product_package;
        strcpy(package->operation_id, "44444444-4444-4444-8444-000000000040");
        package->operation_sequence = 1U;
        package->expected_container_sequence = 6U;
        package->package_size_bytes = 10240U;
        package->guest_abi_version = 2U;
        package->data_schema_version = 1U;
        package->previous_package_present = number >= 50U;
        if (package->previous_package_present)
            memset(package->previous_package_sha256, 0x7a, 32);
        memset(package->package_sha256, 0x7b, 32);
        strcpy(package->package_url, number == 42U ?
            "https://user@packages.example.test/a.pkg" :
             number == 41U || number == 43U ?
                "https://packages.example.test/b.pkg" :
            "https://packages.example.test/a.pkg");
        return NULL;
    }
    snprintf(out->ota.operation_id, sizeof out->ota.operation_id,
             "44444444-4444-4444-8444-%012u", number);
    strcpy(out->ota.image_url, "https://example.invalid/signed.bin");
    out->ota.image_size_bytes = 4096;
    memset(out->ota.sha256, 0x5a, sizeof out->ota.sha256);
    return NULL;
}

static ebase_ledger_io_result_t read_product_ledger(
    void *context, uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    (void)bytes;
    assert(context == &owner);
    if (!product_present) return EBASE_LEDGER_IO_NOT_FOUND;
    memcpy(bytes, product_bytes, sizeof product_bytes);
    return EBASE_LEDGER_IO_OK;
}

static ebase_ledger_io_result_t write_product_ledger(
    void *context, const uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    assert(context == &owner);
    memcpy(product_bytes, bytes, sizeof product_bytes);
    product_present = true;
    return EBASE_LEDGER_IO_OK;
}

ebase_product_ledger_io_t ebase_product_ledger_nvs_io(
    esp_base_storage_owner_t *flash_io_owner)
{
    return (ebase_product_ledger_io_t){read_product_ledger, write_product_ledger,
                                       flash_io_owner};
}

const char *ebase_parse_frp_status(const char *json, size_t length, ebase_request_t *out)
{
    (void)length;
    memset(out, 0, sizeof *out);
    if (!strcmp(json, "invalid")) return "invalid_request";
    strcpy(out->request_id, "11111111-1111-4111-8111-111111111111");
    out->request_id[35] = !strcmp(json, "status-2") ? '2' :
        !strcmp(json, "wrong-boot") ? '3' :
        !strcmp(json, "wrong-device") ? '4' :
        !strcmp(json, "expired") ? '5' :
        !strcmp(json, "far") ? '6' : '1';
    strcpy(out->device_id, !strcmp(json, "wrong-device") ?
        "99999999-9999-4999-8999-999999999999" : "22222222-2222-4222-8222-222222222222");
    strcpy(out->boot_id, (!strcmp(json, "wrong-boot") || !strcmp(json, "changed-boot")) ?
        "88888888-8888-4888-8888-888888888888" : "33333333-3333-4333-8333-333333333333");
    out->expires_at_ms = !strcmp(json, "expired") ? 1000 :
        !strcmp(json, "far") ? 32000 :
        !strcmp(json, "changed-deadline") ? 10001 : 10000;
    return NULL;
}

psa_status_t psa_hash_compute(int algorithm, const uint8_t *bytes, size_t length,
                              uint8_t *out, size_t out_size, size_t *actual)
{
    assert(algorithm == PSA_ALG_SHA_256 && bytes && length && out_size >= 32);
    memset(out, 0xa5, 32);
    for (size_t index = 0; index < length; ++index)
        out[index % 32U] = (uint8_t)((out[index % 32U] * 33U) ^
                                      bytes[index] ^ (uint8_t)index);
    *actual = 32;
    return PSA_SUCCESS;
}

bool eota_available(void) { return true; }
eota_result_t eota_validate_image_request(const eota_image_t *image)
{
    assert(image && image->image_url && image->image_size_bytes > 0);
    ++validate_calls;
    return validate_result;
}
bool esp_base_wifi_ready(void) { return network_ready; }
bool esp_base_time_ready(void) { return trusted_time_ready; }
const char *esp_base_wifi_state(void) { return "ready"; }
const char *esp_base_mqtt_owner_state(void) { return "ready"; }
esp_base_frp_snapshot_t esp_base_frp_owner_snapshot(void) { return (esp_base_frp_snapshot_t){.state = fake_frp_state}; }
uint32_t esp_get_free_heap_size(void) { return fake_free_heap; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 1000; }
#if defined(CONFIG_IDF_TARGET_ESP32)
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT));
    ++iram_work_allocations;
    return malloc(size);
}
#endif
int64_t esp_timer_get_time(void) { return 1000000; }
uint64_t esp_base_mqtt_owner_event_sequence(void) { return 3; }
esp_base_container_event_observation_result_t
esp_base_container_product_event_observation(
    esp_base_container_event_observation_t *out)
{
    *out = (esp_base_container_event_observation_t){0};
    if (fake_event_observation_state == ESP_BASE_CONTAINER_EVENT_OBSERVED) {
        memset(out->package_sha256, 0x11, 32);
        out->event_sequence = 2U;
        out->guest_result = fake_event_guest_result;
        out->runtime_ok = fake_event_runtime_ok;
    }
    return fake_event_observation_state;
}
bool esp_base_mqtt_owner_result(const char *json, size_t length)
{
    assert(length < sizeof latest_reply);
    memcpy(latest_reply, json, length);
    latest_reply[length] = '\0';
    return true;
}
bool esp_base_mqtt_owner_reported(const char *json, size_t length)
{
    assert(length < sizeof latest_reported);
    memcpy(latest_reported, json, length);
    latest_reported[length] = '\0';
    ++reported_calls;
    return true;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_request_t *request,
    const esp_base_ota_receipt_snapshot_t *snapshot)
{
    assert(device_id && request && snapshot && snapshot->source_sha256[0] == 0xa0);
    ++register_calls;
    return register_result;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *receipt)
{
    assert(device_id && receipt);
    ++load_receipt_calls;
    *receipt = (esp_base_ota_receipt_recovery_t){
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .image_size_bytes = s_ota_request.image_size_bytes,
        .container_enabled = product_configured,
        .container_sequence = 7,
    };
    memcpy(receipt->operation_id, s_ota_request.operation_id,
           sizeof receipt->operation_id);
    memcpy(receipt->source_sha256,
           (uint8_t[32]){0xa0}, sizeof receipt->source_sha256);
    memcpy(receipt->candidate_sha256, s_ota_request.sha256,
           sizeof receipt->candidate_sha256);
    return ESP_BASE_OTA_RECEIPT_OK;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error)
{
    assert(device_id && operation_id && error != EOTA_UPDATE_OK);
    ++failure_record_calls;
    return failure_record_result;
}

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack_depth,
                       void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack_depth; (void)priority; (void)handle;
    ++task_calls;
    if (!worker_created) return pdFALSE;
    task(argument);
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void esp_restart(void) { ++restart_calls; }

eota_policy_t esp_base_ota_policy(bool trusted_time)
{
    assert(trusted_time);
    return (eota_policy_t){0};
}
eota_result_t eota_prepare(const eota_policy_t *policy, const eota_image_t *image,
                           eota_progress_t progress, void *context, eota_prepared_t *prepared)
{
    assert(policy && image && progress && prepared);
    (void)context;
    ++prepare_calls;
    return prepare_result;
}
eota_result_t eota_retire_inactive(const eota_policy_t *policy,
    uint8_t target_subtype, const uint8_t source_sha256[EOTA_SHA256_BYTES])
{
    assert(policy && target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
           source_sha256[0] == 0xa0);
    ++retire_calls;
    return retire_result;
}
eota_result_t eota_select(const eota_policy_t *policy, const eota_prepared_t *prepared)
{
    assert(policy && prepared);
    ++select_calls;
    return select_result;
}
const char *eota_error(eota_result_t result)
{
    return result == EOTA_UPDATE_DOWNLOAD_FAILED ? "download_failed" :
        result == EOTA_UPDATE_RESOURCE_FAILURE ? "resource_failure" : "boot_state_unknown";
}

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[37])
{
    assert(esp_base_storage_claim_active(claim) && prepared != NULL &&
           operation_id != NULL && operation_id[0] == '4');
    ++stage_calls;
    return stage_result;
}

bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_receipt_snapshot_t *snapshot)
{
    assert(esp_base_storage_claim_active(claim) && snapshot);
    ++snapshot_calls;
    *snapshot = (esp_base_ota_receipt_snapshot_t){
        .container_enabled = product_configured,
        .container_sequence = 7,
    };
    snapshot->source_sha256[0] = 0xa0;
    return snapshot_ok;
}

esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32])
{
    assert(esp_base_storage_claim_active(claim) &&
           container_enabled == product_configured && expected_sequence == 7 &&
           source_sha256[0] == 0xa0 && inactive_sha256 != NULL);
    ++product_retire_calls;
    return product_retire_result;
}

bool esp_base_container_product_ota_ready(void)
{
    ++ota_ready_calls;
    return product_ota_ready && (ota_ready_calls == 1 || ota_ready_after_first);
}

bool esp_base_container_product_configured(void)
{
    return product_configured;
}

bool esp_base_container_product_pristine_baseline(
    const esp_base_storage_claim_t *claim)
{
    assert(esp_base_storage_claim_active(claim));
    ++pristine_product_calls;
    return pristine_product_baseline;
}

esp_base_container_binding_result_t esp_base_container_product_binding_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *out)
{
    assert(esp_base_storage_claim_active(claim) && out != NULL);
    ++binding_snapshot_calls;
    *out = (esp_base_container_binding_snapshot_t){.container_sequence = binding_sequence,
                                                   .package_present = binding_package_present};
    if (binding_package_present) memset(out->package_sha256, 0x7b, 32);
    return binding_result;
}

esp_base_container_prepare_result_t esp_base_container_product_prepare_package(
    const esp_base_storage_claim_t *claim,
    const esp_base_container_package_request_t *request,
    econtainer_slot_source_fn source_fn, void *source_context,
    uint32_t *prepared_sequence)
{
    assert(esp_base_storage_claim_active(claim) && request != NULL &&
           source_fn != NULL && source_context != NULL &&
           prepared_sequence != NULL && request->expected_sequence == 6U &&
           request->package_sha256[0] == 0x7b);
    ++package_prepare_calls;
    if (package_prepare_result == ESP_BASE_CONTAINER_PREPARE_BUSY) {
        if (package_prepare_busy_snapshot_uncertain)
            binding_result = ESP_BASE_CONTAINER_BINDING_UNCERTAIN;
        return package_prepare_result;
    }
    uint8_t byte = 0U;
    assert(source_fn(source_context, 0U, &byte, 1U) && byte == 0x7b);
    if (package_prepare_result == ESP_BASE_CONTAINER_PREPARED)
        *prepared_sequence = 8U;
    if (package_prepare_result == ESP_BASE_CONTAINER_PREPARE_REJECTED &&
        package_prepare_rejected_after_write) {
        *prepared_sequence = 9U;
        binding_sequence = 9U;
    }
    return package_prepare_result;
}

bool esp_base_container_product_abandon_prepared_package(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[37], const uint8_t package_sha256[32],
    uint32_t *aborted_sequence)
{
    assert(esp_base_storage_claim_active(claim) && prepared_sequence == 8U &&
           operation_id != NULL && package_sha256[0] == 0x7b &&
           aborted_sequence != NULL);
    ++package_abandon_prepared_calls;
    if (package_abandon_prepared_ok) *aborted_sequence = 9U;
    return package_abandon_prepared_ok;
}

bool esp_base_container_product_stop_confirmed(
    const esp_base_storage_claim_t *claim)
{
    assert(esp_base_storage_claim_active(claim));
    ++package_stop_calls;
    return package_stop_ok;
}

esp_base_container_boot_result_t esp_base_container_product_start_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[37], const char boot_id[37])
{
    assert(esp_base_storage_claim_active(claim) && prepared_sequence == 8U &&
           operation_id != NULL && !strcmp(boot_id, s_boot_id));
    ++package_trial_calls;
    if (package_trial_result == ESP_BASE_CONTAINER_RUNNING)
        binding_sequence = prepared_sequence + 1U;
    return package_trial_result;
}

bool esp_base_container_product_abandon_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[37])
{
    assert(esp_base_storage_claim_active(claim) && trial_sequence == 9U &&
           operation_id != NULL);
    ++package_abandon_trial_calls;
    if (package_abandon_trial_ok) binding_sequence = 10U;
    return package_abandon_trial_ok;
}

bool esp_base_container_product_event_accepting(void)
{
    return package_event_accepting;
}

esp_base_container_uninstall_result_t esp_base_container_product_uninstall(
    const esp_base_storage_claim_t *claim, const char operation_id[37],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32])
{
    assert(esp_base_storage_claim_active(claim) && expected_sequence == 6U &&
           expected_package_sha256[0] == 0x7b &&
           !strcmp(operation_id, "44444444-4444-4444-8444-000000000001"));
    ebase_product_ledger_t ledger = {0};
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.count == 1U &&
           ledger.records[0].state == EBASE_PRODUCT_PREPARED);
    ++product_uninstall_calls;
    return product_uninstall_result;
}

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    assert(esp_base_storage_claim_active(claim) && !strcmp(boot_id, s_boot_id));
    ++product_boot_calls;
    return product_boot_result;
}

esp_base_container_uninstall_recovery_t esp_base_container_product_reconcile_uninstall(
    const esp_base_storage_claim_t *claim, const char operation_id[37],
    uint32_t expected_sequence, const uint8_t expected_package_sha256[32])
{
    assert(esp_base_storage_claim_active(claim) && operation_id != NULL &&
           expected_sequence == 6U && expected_package_sha256[0] == 0x7b);
    ++product_recovery_calls;
    return product_recovery_result;
}

esp_base_container_package_recovery_t esp_base_container_product_recover_pending_package(
    const esp_base_storage_claim_t *claim, const char boot_id[37],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    uint32_t expected_sequence, const uint8_t package_sha256[32],
    uint32_t *resolved_sequence)
{
    assert(esp_base_storage_claim_active(claim) && !strcmp(boot_id, s_boot_id) &&
           !strcmp(operation_id, "44444444-4444-4444-8444-000000000010") &&
           expected_sequence == 6U && package_sha256[0] == 0xab &&
           resolved_sequence != NULL);
    ++package_recovery_calls;
    if (package_recovery_outcome != ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN)
        *resolved_sequence = package_recovery_sequence;
    return package_recovery_outcome;
}

bool ebase_config_encode(const esp_base_remote_config_t *config,
                         uint8_t out[EBASE_CONFIG_MAX_BYTES], size_t *written)
{
    (void)config; (void)out; (void)written;
    assert(false && "config.set is outside this test");
    return false;
}
bool esp_base_remote_config_with_canonical_bytes(const esp_base_remote_config_t *config,
                                                 esp_base_config_bytes_consumer_t consume,
                                                 void *context)
{
    assert(config != NULL && config->revision == 7U && config->frp.configured &&
           consume != NULL && context != NULL);
    const uint8_t canonical[] = "config-frp-scratch-candidate";
    return consume(canonical, sizeof canonical - 1U, context);
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    assert(device_id && operation_id && view && !worker_active &&
           !strcmp(operation_id, "44444444-4444-4444-8444-000000000022"));
    ++query_calls;
    return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
}
esp_err_t esp_base_wifi_apply(const ebase_wifi_config_t *config, uint64_t now_ms)
{
    (void)config; (void)now_ms;
    ++wifi_apply_calls;
    return ESP_OK;
}
esp_err_t esp_base_remote_config_commit_verified(const esp_base_remote_config_t *candidate,
                                                 uint32_t expected_revision,
                                                 esp_base_remote_config_t *committed,
                                                 esp_base_remote_config_t *work)
{
    assert(candidate != NULL && expected_revision == 7U && committed != NULL && work != NULL);
    ++config_commit_calls;
    *committed = *candidate;
    committed->revision = expected_revision + 1U;
    return ESP_OK;
}
esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    (void)config;
    assert(false && "successful config commit must not reload storage");
    return ESP_FAIL;
}
psa_status_t psa_hash_setup(psa_hash_operation_t *operation, int algorithm)
{
    assert(operation != NULL && algorithm == PSA_ALG_SHA_256);
    operation->sum = 0U;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_update(psa_hash_operation_t *operation, const uint8_t *bytes, size_t length)
{
    assert(operation != NULL && bytes != NULL && length > 0U);
    for (size_t i = 0; i < length; ++i) operation->sum += bytes[i];
    return PSA_SUCCESS;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *operation, uint8_t *out, size_t out_size, size_t *actual)
{
    assert(operation != NULL && out != NULL && out_size >= 32U && actual != NULL);
    memset(out, (uint8_t)operation->sum, 32U);
    *actual = 32U;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *operation)
{
    assert(operation != NULL);
    return PSA_SUCCESS;
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    assert(false && "restart is outside this test");
}
