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
static bool product_ledger_read_busy;
static bool pristine_product_baseline;
static unsigned pristine_product_calls;
static esp_base_container_binding_result_t binding_result;
static unsigned active_fixture_mode;
static bool binding_package_present;
static unsigned binding_snapshot_calls;
static esp_base_container_uninstall_result_t product_uninstall_result;
static esp_base_container_boot_result_t product_boot_result;
static unsigned product_uninstall_calls, product_boot_calls;
static esp_base_container_run_result_t product_run_result;
static unsigned product_run_calls;
static bool product_run_started;
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
static bool network_ready, trusted_time_ready, mqtt_ready;
static esp_base_container_boot_result_t package_trial_result;
static bool package_stop_ok, package_abandon_prepared_ok;
static bool package_event_accepting, package_abandon_trial_ok;
static bool package_confirm_ok;
static bool package_confirm_not_started;
static unsigned package_confirm_calls;
static uint8_t fake_observed_package_byte, fake_observed_event_byte;
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
static esp_base_ota_package_mode_t loaded_package_mode;
static char latest_reply[5121];
static char latest_reported[768];
static unsigned reported_calls;
static unsigned wifi_apply_calls, config_commit_calls, config_load_calls;
static bool allow_config_load;
static esp_err_t config_commit_result, config_load_result;
static esp_base_remote_config_t config_load_output;
static const char *fake_frp_state = "stopped";
static uint32_t fake_free_heap = 1000;
static esp_base_container_event_observation_result_t fake_event_observation_state;
static int32_t fake_event_guest_result;
static bool fake_event_runtime_ok;
static bool fake_trial_snapshot_ready;
static uint64_t fake_trial_representative_sequence, fake_trial_failure_count;
static unsigned offered_event_calls;
static uint8_t offered_event_digest[32];
static esp_base_container_event_result_t offered_event_result;
static uint64_t fake_now_ms;
static bool fake_trial_quiescent;
static unsigned mqtt_configures, mqtt_polls, mqtt_revokes;
static unsigned frp_configures, frp_polls, listener_configures, listener_polls;
static bool fake_frp_response_pending;
static bool mqtt_restart_enqueue_ok, mqtt_restart_acknowledged;
static unsigned mqtt_restart_result_calls;
const char *ebase_parse_command_real(const char *, size_t, ebase_command_t *, ebase_command_alloc_t);
const char *ebase_parse_frp_status_real(const char *, size_t, ebase_request_t *);
static esp_err_t mqtt_revoke_result;
static bool ota_source_package_present, ota_source_snapshot_changed, ota_package_receipt_changed;
static esp_base_container_stage_result_t ota_package_write_result;
static unsigned ota_package_write_calls;
#if defined(CONFIG_IDF_TARGET_ESP32)
static unsigned iram_work_allocations;
#endif

static void reset_case(void)
{
    ebase_line_release(s_reader);
    free(s_reader);
    s_reader = NULL;
    s_serial_discard = false;
    esp_base_storage_owner_init(&owner);
    memset(&s_context, 0, sizeof s_context);
    s_context.device_id = "22222222-2222-4222-8222-222222222222";
    s_context.storage_owner = &owner;
    s_context.flash_io_owner = &owner;
    product_present = false;
    product_ledger_read_busy = false;
    pristine_product_baseline = false;
    pristine_product_calls = 0;
    binding_result = ESP_BASE_CONTAINER_BINDING_OK;
    active_fixture_mode = 0U;
    binding_package_present = false;
    binding_snapshot_calls = 0;
    product_uninstall_result = ESP_BASE_CONTAINER_UNINSTALL_COMPLETE;
    product_boot_result = ESP_BASE_CONTAINER_EMPTY;
    product_uninstall_calls = product_boot_calls = 0;
    product_run_calls = 0U;
    product_run_started = false;
    product_run_result = ESP_BASE_CONTAINER_RUN_COMPLETE;
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
    network_ready = trusted_time_ready = mqtt_ready = true;
    package_trial_result = ESP_BASE_CONTAINER_RUNNING;
    package_stop_ok = package_abandon_prepared_ok = true;
    package_event_accepting = true;
    package_confirm_ok = true;
    package_confirm_not_started = false;
    package_confirm_calls = 0U;
    fake_observed_package_byte = 0x11U;
    fake_observed_event_byte = 0x22U;
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
    free(s_product_run_request);
    s_product_run_request = NULL;
    memset(&s_product_storage_claim, 0, sizeof s_product_storage_claim);
    memset(s_product_operation_id, 0, sizeof s_product_operation_id);
    memset(s_product_fingerprint, 0, sizeof s_product_fingerprint);
    s_product_operation_sequence = s_product_trial_sequence = 0U;
    memset(s_product_trial_event_sha256, 0, sizeof s_product_trial_event_sha256);
    memset(s_product_trial_package_sha256, 0, sizeof s_product_trial_package_sha256);
    s_product_trial_event_sequence = s_product_trial_stable_since_ms =
        s_product_trial_last_poll_ms = 0U;
    s_product_trial_failure_count = 0U;
    s_product_active = s_product_trial_running = false;
    s_started = false;
    s_pending_mqtt_active = s_mqtt_revision_set = s_frp_revision_set = false;
    s_frp_restart_pending = fake_frp_response_pending = false;
    s_frp_restart_since_ms = 0U;
    s_mqtt_restart_pending = mqtt_restart_acknowledged = false;
    s_mqtt_restart_since_ms = 0U;
    mqtt_restart_enqueue_ok = true;
    mqtt_restart_result_calls = 0U;
    atomic_store(&s_firmware_package_verifying, false);
    atomic_flag_clear(&s_firmware_health_lock);
    memset(&s_firmware_health, 0, sizeof s_firmware_health);
    fake_now_ms = 1000U;
    fake_trial_quiescent = true;
    mqtt_configures = mqtt_polls = mqtt_revokes = 0U;
    frp_configures = frp_polls = listener_configures = listener_polls = 0U;
    mqtt_revoke_result = ESP_OK;
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
    loaded_package_mode = ESP_BASE_OTA_NO_PACKAGE;
    ota_source_package_present = ota_source_snapshot_changed = ota_package_receipt_changed = false;
    ota_package_write_result = ESP_BASE_CONTAINER_STAGE_PREPARED;
    ota_package_write_calls = 0U;
    validate_calls = query_calls = 0;
    latest_reply[0] = '\0';
    latest_reported[0] = '\0';
    reported_calls = 0;
    fake_frp_state = "stopped";
    fake_free_heap = 1000;
    fake_event_observation_state = ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION;
    fake_event_guest_result = 3;
    fake_event_runtime_ok = true;
    fake_trial_snapshot_ready = false;
    fake_trial_representative_sequence = fake_trial_failure_count = 0U;
#if defined(CONFIG_IDF_TARGET_ESP32)
    iram_work_allocations = 0;
#endif
    wifi_apply_calls = config_commit_calls = config_load_calls = 0;
    allow_config_load = false;
    config_commit_result = config_load_result = ESP_OK;
    config_load_output = (esp_base_remote_config_t){.revision = 7U};
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


static void product_run_json(unsigned id, bool running, uint32_t sequence,
    const char *device, const char *boot, uint64_t deadline, unsigned digest_byte)
{
    char digest[65], json[700];
    for (size_t i = 0; i < 32U; ++i)
        (void)snprintf(digest + i * 2U, 3U, "%02x", digest_byte);
    const int length = snprintf(json, sizeof json,
        "{\"protocol_version\":1,\"request_id\":\"55555555-5555-4555-8555-%012u\","
        "\"command\":\"product.%s\",\"device_id\":\"%s\",\"target_boot_id\":\"%s\","
        "\"expires_at_uptime_ms\":%llu,\"parameters\":{\"expected_container_sequence\":%u,"
        "\"package_sha256\":\"%s\"}}", id, running ? "start" : "stop", device,
        boot, (unsigned long long)deadline, sequence, digest);
    assert(length > 0 && (size_t)length < sizeof json);
    s_reply_mqtt = true;
    handle_line(json, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void product_run(unsigned id, bool running)
{
    product_run_json(id, running, binding_sequence, s_context.device_id,
        s_boot_id, 31000U, 0x7bU);
}

static void product_run_query(unsigned id)
{
    char json[300];
    const int length = snprintf(json, sizeof json,
        "{\"protocol_version\":1,\"request_id\":\"66666666-6666-4666-8666-000000000001\","
        "\"command\":\"product.result\",\"parameters\":{\"operation_id\":"
        "\"55555555-5555-4555-8555-%012u\"}}", id);
    assert(length > 0 && (size_t)length < sizeof json);
    s_reply_mqtt = true;
    handle_line(json, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void setup_product_run(void)
{
    reset_case();
    product_configured = true;
    binding_package_present = true;
    initialize_empty_product_ledger();
}

static void check_product_run_guard(void)
{
    setup_product_run();
    uint8_t ledger_before[sizeof product_bytes];
    memcpy(ledger_before, product_bytes, sizeof product_bytes);
    product_run(1U, false);
    expect_reply("running", NULL);
    assert(product_run_calls == 1U && !product_run_started && s_product_active);
    product_run_query(1U);
    expect_reply("running", NULL);
    assert(strstr(latest_reply, "\"operation_sequence\":null") &&
        strstr(latest_reply, "\"kind\":\"stop\"") &&
        strstr(latest_reply, "\"container_sequence\":6") &&
        !strstr(latest_reply, "package_sha256") && !strstr(latest_reply, "result_code"));
    poll_product();
    expect_reply("succeeded", NULL);
    assert(!s_product_active && !s_product_run_request && atomic_load(&owner.active_token) == 0U);
    product_run(1U, false);
    expect_reply("succeeded", NULL);
    assert(product_run_calls == 1U && task_calls == 1U);
    product_run(1U, true);
    expect_reply("failed", "request_conflict");
    product_run_json(1U, false, 7U, s_context.device_id, s_boot_id, 31000U, 0x7bU);
    expect_reply("failed", "request_conflict");
    product_run_json(1U, false, 6U, s_context.device_id, s_boot_id, 31000U, 0x7cU);
    expect_reply("failed", "request_conflict");
    product_run_json(1U, false, 6U, s_context.device_id, s_boot_id, 31001U, 0x7bU);
    expect_reply("failed", "request_conflict");
    product_run_json(1U, false, 6U, s_context.device_id,
        "77777777-7777-4777-8777-777777777777", 31000U, 0x7bU);
    expect_reply("failed", "wrong_boot");
    product_run_json(1U, false, 6U, "77777777-7777-4777-8777-777777777777",
        s_boot_id, 31000U, 0x7bU);
    expect_reply("failed", "wrong_device");
    assert(product_run_calls == 1U && s_guard.count == 1U);
    product_run(2U, true);
    expect_reply("running", NULL);
    poll_product();
    expect_reply("succeeded", NULL);
    assert(product_run_started && product_run_calls == 2U &&
        memcmp(ledger_before, product_bytes, sizeof product_bytes) == 0);

    /* A later durable write cannot take an existing boot-local operation ID. */
    for (unsigned kind = 0U; kind < 3U; ++kind) {
        char json[1200], digest[65];
        for (size_t i = 0; i < 32U; ++i) (void)snprintf(digest + i * 2U, 3U, "7b");
        const int length = snprintf(json, sizeof json,
            "{\"protocol_version\":1,\"request_id\":\"88888888-8888-4888-8888-%012u\","
            "\"command\":\"product.%s\",\"device_id\":\"%s\",\"target_boot_id\":\"%s\","
            "\"expires_at_uptime_ms\":31000,\"parameters\":{\"operation_id\":"
            "\"55555555-5555-4555-8555-000000000001\",\"operation_sequence\":1,"
            "\"expected_container_sequence\":6,\"package_sha256\":\"%s\"%s}}",
            kind + 1U, kind == 0U ? "install" : kind == 1U ? "upgrade" : "uninstall",
            s_context.device_id, s_boot_id, digest, kind == 2U ? "" :
            ",\"previous_package_sha256\":null,\"package_url\":\"https://packages.example.test/a.pkg\","
            "\"trial_event_sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\","
            "\"package_size_bytes\":10240,\"guest_abi_version\":2,\"data_schema_version\":1");
        assert(length > 0 && (size_t)length < sizeof json);
        /* Upgrade needs a present old digest; install needs null. */
        if (kind == 1U) {
            char *field = strstr(json, "\"previous_package_sha256\":null");
            assert(field != NULL);
            const size_t offset = (size_t)(field - json) + strlen("\"previous_package_sha256\":");
            const size_t tail = strlen(json + offset + 4U);
            memmove(json + offset + 66U, json + offset + 4U, tail + 1U);
            json[offset] = '"'; memcpy(json + offset + 1U, digest, 64U); json[offset + 65U] = '"';
        }
        s_reply_mqtt = true; handle_line(json, strlen(json), NULL); s_reply_mqtt = false;
        expect_reply("failed", "product_operation_conflict");
        product_run_query(1U);
        expect_reply("succeeded", NULL);
        assert(strstr(latest_reply, "\"kind\":\"stop\"") &&
            memcmp(ledger_before, product_bytes, sizeof product_bytes) == 0 &&
            product_uninstall_calls == 0U && package_prepare_calls == 0U);
    }
    s_guard.count = EBASE_REQUEST_SLOTS;
    product_run(3U, false);
    expect_reply("failed", "capacity_exceeded");
    product_run_query(1U);
    expect_reply("succeeded", NULL);
    memset(&s_guard, 0, sizeof s_guard);
    memset(s_outcomes, 0, sizeof s_outcomes);
    strcpy(s_boot_id, "77777777-7777-4777-8777-777777777777");
    product_run_query(1U);
    expect_reply("unknown", "product_operation_not_found");
    assert(strstr(latest_reply, "\"result\":null") && product_run_calls == 2U);

    setup_product_run();
    product_run_json(4U, false, 6U, s_context.device_id, s_boot_id, 1000U, 0x7bU);
    expect_reply("expired", "expired");
    assert(s_guard.count == 0U && product_run_calls == 0U);
    product_run_query(4U);
    expect_reply("unknown", "product_operation_not_found");

    for (unsigned gate = 0; gate < 6U; ++gate) {
        setup_product_run();
        if (gate == 0U) esp_base_control_state_set_ota_pending(&s_control_state, true);
        if (gate == 1U) s_ota_active = true;
        if (gate == 2U) s_trial_active = true;
        if (gate == 3U) s_product_active = true;
        if (gate == 4U) s_config_uncertain = true;
        if (gate == 5U) binding_sequence = 7U;
        product_run_json(5U, false, 6U, s_context.device_id, s_boot_id, 31000U, 0x7bU);
        expect_reply(gate == 4U ? "unknown" : "failed",
            gate == 0U ? "ota_verification_pending" : gate == 4U ? "storage_uncertain" :
            gate == 5U ? "product_precondition_conflict" : "operation_busy");
        product_run_query(5U);
        assert(strstr(latest_reply, "\"operation_sequence\":null") && product_run_calls == 0U);
    }

    setup_product_run();
    ebase_command_t old_command = {0};
    assert(ebase_parse_command("package-43", strlen("package-43"), &old_command, malloc) == NULL);
    assert(fingerprint_product_package(&old_command, s_product_fingerprint));
    strcpy(s_product_operation_id, old_command.product_package->operation_id);
    ebase_command_release(&old_command);
    product_run(6U, false); /* The new active worker is stop, not the old package. */
    product_package(43U);
    expect_reply("failed", "operation_busy");
    assert(product_run_calls == 1U && package_source_opens == 0U);
    poll_product();
    product_run_query(6U);
    expect_reply("succeeded", NULL);
    setup_product_run();
    worker_created = false;
    product_run(6U, true);
    expect_reply("failed", "resource_failure");
    product_run_query(6U);
    expect_reply("failed", "resource_failure");
    assert(!s_product_run_request && !s_product_active && product_run_calls == 0U &&
        atomic_load(&owner.active_token) == 0U);

    const esp_base_container_run_result_t outcomes[] = {
        ESP_BASE_CONTAINER_RUN_REJECTED, ESP_BASE_CONTAINER_RUN_BUSY,
        ESP_BASE_CONTAINER_RUN_UNCERTAIN};
    for (size_t index = 0; index < sizeof outcomes / sizeof *outcomes; ++index) {
        setup_product_run();
        product_run_result = outcomes[index];
        product_run(7U, false);
        poll_product();
        product_run_query(7U);
        expect_reply(index == 2U ? "unknown" : "failed", index == 2U ?
            "product_state_uncertain" : index == 1U ? "operation_busy" :
            "product_precondition_conflict");
        product_run(7U, false);
        assert(product_run_calls == 1U);
        if (index == 2U) {
            assert(s_product_active && s_config_uncertain &&
                esp_base_storage_claim_active(&s_product_storage_claim));
            product_run(8U, true);
            assert(product_run_calls == 1U);
            product_run_query(7U);
            expect_reply("unknown", "product_state_uncertain");
        } else assert(!s_product_active && atomic_load(&owner.active_token) == 0U);
    }
    setup_product_run();
    esp_base_storage_claim_t flash_busy = {0};
    assert(esp_base_storage_claim(&owner, &flash_busy));
    product_ledger_read_busy = true;
    product_run(9U, false);
    expect_reply("unknown", "operation_busy");
    assert(!s_outcomes[0].has_product_run && product_run_calls == 0U);
    assert(esp_base_storage_release(&flash_busy));
    product_ledger_read_busy = false;
    product_run_query(9U);
    expect_reply("unknown", "product_operation_not_found");

    /* Durable IDs retain their original six-field query result. */
    setup_product_run();
    ebase_product_ledger_t ledger;
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    ebase_product_record_t record = {.sequence = 1U, .container_sequence = 6U,
        .kind = EBASE_PRODUCT_UNINSTALL, .state = EBASE_PRODUCT_PREPARED};
    strcpy(record.operation_id, "55555555-5555-4555-8555-000000000010");
    memset(record.fingerprint, 0xa1, 32U);
    memset(record.package_sha256, 0x7b, 32U);
    assert(ebase_product_ledger_begin(&ledger, &io, &record) == EBASE_LEDGER_OK);
    assert(ebase_product_ledger_finish(&ledger, &io, 1U, record.operation_id,
        record.fingerprint, EBASE_PRODUCT_FAILED, 1U, 6U) == EBASE_LEDGER_OK);
    memcpy(ledger_before, product_bytes, sizeof product_bytes);
    product_run(10U, false);
    expect_reply("failed", "product_operation_conflict");
    assert(!s_outcomes[0].has_product_run && product_run_calls == 0U);
    product_run_query(10U);
    assert(strstr(latest_reply, "\"operation_sequence\":1") &&
        strstr(latest_reply, "\"kind\":\"uninstall\"") &&
        strstr(latest_reply, "\"result_code\":1") &&
        memcmp(ledger_before, product_bytes, sizeof product_bytes) == 0);
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
    product_package(46U); /* Same request ID, different representative event. */
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

static void check_product_trial_health(void)
{
    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    product_package(40U);
    expect_reply("running", NULL);
    poll_product();
    assert(s_product_trial_running && !atomic_load(&owner.active_token));
    fake_event_observation_state = ESP_BASE_CONTAINER_EVENT_OBSERVED;
    fake_trial_snapshot_ready = true;
    fake_observed_package_byte = 0x7bU;
    fake_observed_event_byte = 0x33U;
    for (uint64_t now = 1000U; now <= 16000U; now += 1000U)
        poll_product_trial_health(now);
    assert(package_confirm_calls == 0U);
    fake_observed_event_byte = 0x22U;
    fake_trial_representative_sequence = 2U;
    for (uint64_t now = 17000U; now <= 32000U; now += 1000U)
        poll_product_trial_health(now);
    assert(package_confirm_calls == 0U);
    mqtt_ready = false;
    poll_product_trial_health(33000U);
    mqtt_ready = true;
    for (uint64_t now = 34000U; now < 64000U; now += 1000U) {
        if (now == 50000U) fake_observed_event_byte = 0x33U;
        poll_product_trial_health(now);
    }
    assert(package_confirm_calls == 0U);
    package_confirm_not_started = true;
    poll_product_trial_health(64000U);
    assert(package_confirm_calls == 1U && s_product_active &&
           s_product_trial_running && !s_config_uncertain &&
           !atomic_load(&owner.active_token));
    package_confirm_not_started = false;
    poll_product_trial_health(65000U);
    expect_reply("succeeded", NULL);
    assert(package_confirm_calls == 2U && !s_product_active &&
           !s_product_trial_running && !atomic_load(&owner.active_token));
    ebase_product_ledger_t ledger = {0};
    const ebase_product_ledger_io_t io = ebase_product_ledger_nvs_io(&owner);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK &&
           ledger.records[0].state == EBASE_PRODUCT_SUCCEEDED &&
           ledger.records[0].container_sequence == 11U);

    reset_case();
    product_configured = true;
    initialize_empty_product_ledger();
    product_package(40U);
    poll_product();
    fake_event_observation_state = ESP_BASE_CONTAINER_EVENT_OBSERVED;
    fake_trial_snapshot_ready = true;
    fake_observed_package_byte = 0x7bU;
    fake_trial_representative_sequence = 2U;
    package_confirm_ok = false;
    for (uint64_t now = 1000U; now <= 10000U; now += 1000U)
        poll_product_trial_health(now);
    fake_trial_failure_count = 1U;
    fake_trial_representative_sequence = 0U;
    poll_product_trial_health(11000U);
    fake_trial_representative_sequence = 2U;
    for (uint64_t now = 12000U; now <= 42000U; now += 1000U)
        poll_product_trial_health(now);
    expect_reply("unknown", "storage_uncertain");
    assert(package_confirm_calls == 1U && s_config_uncertain &&
           s_product_active && !s_product_trial_running &&
           atomic_load(&owner.active_token) != 0U);
}

static void firmware_health_poll(uint64_t now)
{
    fake_now_ms = now;
    poll_network_owners(now);
    esp_base_control_state_note_progress(&s_control_state);
}

static void firmware_health_window(uint64_t first_ms)
{
    esp_base_protocol_firmware_package_health_t health = {0};
    for (uint64_t now = first_ms; now < first_ms + 30000U; now += 1000U) {
        firmware_health_poll(now);
        assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    }
    firmware_health_poll(first_ms + 30000U);
    assert(esp_base_protocol_firmware_package_health_snapshot(&health) &&
           health.event_sequence == fake_trial_representative_sequence &&
           health.failure_count == fake_trial_failure_count);
}

static void check_firmware_package_health(void)
{
    reset_case();
    uint8_t digest[32];
    memset(digest, fake_observed_package_byte, sizeof digest);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    s_started = true;
    esp_base_protocol_set_ota_verification_pending(true);
    firmware_health_poll(1000U);
    assert(mqtt_configures == 0U && mqtt_polls == 0U && frp_polls == 0U);
    assert(!esp_base_protocol_begin_firmware_package_verification(NULL, digest));
    assert(esp_base_protocol_begin_firmware_package_verification(&claim, digest));
    assert(!esp_base_protocol_begin_firmware_package_verification(&claim, digest));
    firmware_health_poll(2000U);
    assert(mqtt_configures == 1U && mqtt_polls == 1U && frp_configures == 0U &&
           frp_polls == 0U && listener_polls == 0U);
    esp_base_protocol_firmware_package_health_t health = {0};
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    fake_trial_snapshot_ready = true;
    fake_trial_representative_sequence = 5U;
    firmware_health_window(3000U);
    assert(esp_base_control_state_config_write_error(&s_control_state) &&
           !strcmp(esp_base_control_state_config_write_error(&s_control_state),
                   "ota_verification_pending"));
    fake_now_ms += 1001U;
    esp_base_control_state_note_progress(&s_control_state);
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    firmware_health_poll(fake_now_ms); /* excessive poll gap restarts the window */
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    firmware_health_window(fake_now_ms + 1000U);
    assert(!atomic_flag_test_and_set(&s_firmware_health_lock));
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    atomic_flag_clear(&s_firmware_health_lock);
    assert(esp_base_protocol_firmware_package_health_snapshot(&health));

    /* Each external predicate and a failed event resets the full window. */
    bool *predicates[] = {&network_ready, &trusted_time_ready, &mqtt_ready,
                         &package_event_accepting, &fake_trial_snapshot_ready};
    for (size_t index = 0; index < sizeof predicates / sizeof predicates[0]; ++index) {
        *predicates[index] = false;
        firmware_health_poll(fake_now_ms + 1000U);
        assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
        *predicates[index] = true;
        firmware_health_window(fake_now_ms + 1000U);
    }
    fake_observed_package_byte = 0x7fU;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    fake_observed_package_byte = digest[0];
    firmware_health_window(fake_now_ms + 1000U);
    ++fake_trial_failure_count;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    firmware_health_window(fake_now_ms + 1000U);
    fake_trial_failure_count = UINT64_MAX;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    fake_trial_failure_count = 2U;
    firmware_health_poll(fake_now_ms + 1000U);
    firmware_health_window(fake_now_ms + 1000U);
    fake_trial_quiescent = false;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    fake_trial_quiescent = true;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(esp_base_protocol_firmware_package_health_snapshot(&health));
    firmware_health_poll(fake_now_ms - 1000U);
    assert(!esp_base_protocol_firmware_package_health_snapshot(&health));
    firmware_health_window(fake_now_ms + 1000U);

    /* Aborting keeps the write gate and revokes MQTT in its owning task;
     * incomplete cleanup is retried, with no FRP admission. */
    esp_base_protocol_end_firmware_package_verification();
    mqtt_revoke_result = ESP_ERR_TIMEOUT;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(mqtt_revokes == 1U && s_pending_mqtt_active && frp_polls == 0U);
    mqtt_revoke_result = ESP_OK;
    firmware_health_poll(fake_now_ms + 1000U);
    assert(mqtt_revokes == 2U && !s_pending_mqtt_active && !s_mqtt_revision_set &&
           frp_polls == 0U && !esp_base_protocol_firmware_package_health_snapshot(&health));

    /* The successful pending -> normal transition preserves its MQTT session. */
    assert(esp_base_protocol_begin_firmware_package_verification(&claim, digest));
    firmware_health_poll(fake_now_ms + 1000U);
    assert(mqtt_configures == 2U);
    esp_base_protocol_set_ota_verification_pending(false);
    esp_base_protocol_end_firmware_package_verification();
    firmware_health_poll(fake_now_ms + 1000U);
    assert(mqtt_configures == 2U && mqtt_revokes == 2U && frp_configures == 1U &&
           frp_polls == 1U && listener_configures == 2U && listener_polls == 1U);
    assert(esp_base_storage_release(&claim));
}

static void start_package_ota(unsigned number, bool source_present)
{
    reset_case();
    product_configured = true;
    ota_source_package_present = source_present;
    loaded_package_mode = number == 85U ? ESP_BASE_OTA_PACKAGE_REUSE : ESP_BASE_OTA_PACKAGE_WRITE;
    stage_result = number == 85U ? ESP_BASE_CONTAINER_STAGE_PREPARED : ESP_BASE_CONTAINER_STAGE_WRITING;
}

static void check_package_ota_worker(void)
{
    for (unsigned number = 85U; number <= 86U; ++number) {
        start_package_ota(number, true);
        start(number);
        assert(register_calls == 1U && snapshot_calls == 2U && retire_calls == 1U &&
               product_retire_calls == 1U && prepare_calls == 1U && package_stop_calls == 1U &&
               stage_calls == 1U && select_calls == 1U &&
               ota_package_write_calls == (number == 86U ? 1U : 0U) &&
               package_source_opens == (number == 86U ? 1U : 0U) &&
               package_source_closes == package_source_opens);
        poll_ota();
        assert(restart_calls == 1U && failure_record_calls == 0U &&
               esp_base_storage_claim_active(&s_ota_storage_claim));
        start(number); /* Same admitted request does not prepare again. */
        assert(register_calls == 1U && prepare_calls == 1U && select_calls == 1U);

        start_package_ota(number, true);
        ota_package_receipt_changed = true;
        start(number);
        poll_ota();
        expect_reply("unknown", "storage_uncertain");
        assert(retire_calls == 0U && stage_calls == 0U && select_calls == 0U &&
               failure_record_calls == 0U && esp_base_storage_claim_active(&s_ota_storage_claim));

        start_package_ota(number, true);
        ota_source_snapshot_changed = true;
        start(number);
        poll_ota();
        expect_reply("unknown", "storage_uncertain");
        assert(snapshot_calls == 2U && retire_calls == 0U && select_calls == 0U);

        start_package_ota(number, true);
        package_stop_ok = false;
        start(number);
        poll_ota();
        expect_reply("unknown", "storage_uncertain");
        assert(prepare_calls == 1U && package_stop_calls == 1U && stage_calls == 0U &&
               select_calls == 0U && failure_record_calls == 0U &&
               esp_base_storage_claim_active(&s_ota_storage_claim));

        start_package_ota(number, true);
        stage_result = ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
        start(number);
        poll_ota();
        expect_reply("unknown", "storage_uncertain");
        assert(stage_calls == 1U && ota_package_write_calls == 0U && select_calls == 0U);

        start_package_ota(number, true);
        worker_created = false;
        start(number);
        expect_reply("failed", "resource_failure");
        assert(failure_record_calls == 1U && retire_calls == 0U && package_stop_calls == 0U &&
               !atomic_load(&owner.active_token));

        start_package_ota(number, true);
        esp_base_protocol_set_ota_verification_pending(true);
        start(number);
        expect_reply("failed", "ota_verification_pending");
        assert(register_calls == 0U && prepare_calls == 0U && retire_calls == 0U);
    }
    start_package_ota(86U, false);
    start(86U);
    poll_ota();
    assert(restart_calls == 1U && package_stop_calls == 0U && ota_package_write_calls == 1U);
    for (unsigned fault = 0U; fault < 3U; ++fault) {
        start_package_ota(86U, true);
        if (fault == 0U) package_source_open_ok = false;
        if (fault == 1U) package_source_complete_ok = false;
        if (fault == 2U) ota_package_write_result = ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
        start(86U);
        poll_ota();
        expect_reply("unknown", "storage_uncertain");
        assert(select_calls == 0U && restart_calls == 0U && failure_record_calls == 0U &&
               package_source_closes == (fault == 0U ? 0U : 1U) &&
               esp_base_storage_claim_active(&s_ota_storage_claim));
    }
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
                             const char *expected_error, unsigned expected_heap)
{
    /* Actual listener body capacity after reserving the authenticated header. */
    char response[768] = {0};
    size_t length = 0;
    const int http = handle_frp_status((const uint8_t *)request, strlen(request),
                                       response, sizeof response, &length, NULL);
    assert(http == expected_http && length > 0 && length < sizeof response);
    if (expected_error) assert(strstr(response, expected_error));
    else {
        char heap_field[48];
        snprintf(heap_field, sizeof heap_field, "\"free_heap\":%u", expected_heap);
        assert(strstr(response, "\"state\":\"succeeded\"") &&
               strstr(response, "\"frp\":\"stopped\"") && strstr(response, heap_field) &&
               strstr(response, "\"device_id\":\"22222222-2222-4222-8222-222222222222\"") &&
               strstr(response, "\"boot_id\":\"33333333-3333-4333-8333-333333333333\""));
    }
}


static void restart_request(char out[384], unsigned number, const char *device,
                             const char *boot, unsigned deadline)
{
    const int n = snprintf(out, 384,
        "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-%012u\","
        "\"command\":\"restart\",\"device_id\":\"%s\",\"target_boot_id\":\"%s\","
        "\"expires_at_uptime_ms\":%u,\"parameters\":{}}", number, device, boot, deadline);
    assert(n > 0 && n < 384);
}

static void expect_frp_restart(const char *request, int expected_http,
                               const char *state, const char *error)
{
    char response[768];
    size_t length = 0;
    assert(handle_frp_management(ESP_BASE_FRP_MANAGEMENT_RESTART,
        (const uint8_t *)request, strlen(request), response, sizeof response,
        &length, NULL) == expected_http);
    assert(length > 0 && length < sizeof response);
    assert(strstr(response, state) && strstr(response, "\"result\":null"));
    if (error) assert(strstr(response, error));
    else assert(strstr(response, "\"error_code\":null"));
}

static void check_mqtt_restart(void)
{
    const char *device = "22222222-2222-4222-8222-222222222222";
    const char *boot = "33333333-3333-4333-8333-333333333333";
    char request[384], second[384];
    reset_case();
    restart_request(request, 1U, device, boot, 31000U);
    handle_mqtt_command((const uint8_t *)request, strlen(request), NULL);
    expect_reply("running", NULL);
    assert(s_mqtt_restart_pending && restart_calls == 0U && mqtt_restart_result_calls == 1U);
    handle_mqtt_command((const uint8_t *)request, strlen(request), NULL);
    expect_reply("running", NULL);
    assert(mqtt_restart_result_calls == 1U && s_mqtt_restart_since_ms == 1000U);
    restart_request(second, 2U, device, boot, 31000U);
    handle_mqtt_command((const uint8_t *)second, strlen(second), NULL);
    expect_reply("failed", "operation_busy");
    expect_frp_restart(second, 409, "failed", "operation_busy");
    poll_mqtt_restart(1099U);
    assert(restart_calls == 0U);
    mqtt_restart_acknowledged = true;
    poll_mqtt_restart(1099U);
    assert(restart_calls == 0U);
    poll_mqtt_restart(1100U);
    poll_mqtt_restart(1101U);
    assert(restart_calls == 1U && !s_mqtt_restart_pending);

    reset_case();
    handle_mqtt_command((const uint8_t *)request, strlen(request), NULL);
    poll_mqtt_restart(1100U);
    poll_mqtt_restart(2999U);
    assert(restart_calls == 0U && s_mqtt_restart_pending);
    poll_mqtt_restart(3000U);
    poll_mqtt_restart(3001U);
    assert(restart_calls == 1U && !s_mqtt_restart_pending);

    reset_case();
    mqtt_restart_enqueue_ok = false;
    handle_mqtt_command((const uint8_t *)request, strlen(request), NULL);
    expect_reply("failed", "resource_failure");
    poll_mqtt_restart(4000U);
    assert(!s_mqtt_restart_pending && restart_calls == 0U);

    reset_case();
    handle_line(request, strlen(request), NULL);
    assert(restart_calls == 1U && !s_mqtt_restart_pending && mqtt_restart_result_calls == 0U);
}

static void check_frp_restart(void)
{
    const char *device = "22222222-2222-4222-8222-222222222222";
    const char *boot = "33333333-3333-4333-8333-333333333333";
    const char *other = "99999999-9999-4999-8999-999999999999";
    char request[384], second[384];
    reset_case();
    restart_request(request, 1U, device, boot, 31000U);
    expect_frp_restart(request, 202, "running", NULL);
    assert(s_guard.count == 1U && s_frp_restart_pending && restart_calls == 0U);
    fake_now_ms = 1010U;
    expect_frp_restart(request, 202, "running", NULL);
    assert(s_guard.count == 1U && s_frp_restart_since_ms == 1000U);
    restart_request(second, 2U, device, boot, 31000U);
    expect_frp_restart(second, 409, "failed", "operation_busy");
    handle_mqtt_command((const uint8_t *)second, strlen(second), NULL);
    expect_reply("failed", "operation_busy");
    assert(s_guard.count == 2U && restart_calls == 0U);
    /* All admitted writes stop before persistence, task creation or USB reply. */
    start(3U);
    expect_reply("failed", "operation_busy");
    handle_mqtt_command((const uint8_t *)"package-40", strlen("package-40"), NULL);
    expect_reply("failed", "operation_busy");
    char usb[1200];
    config_set(20U, false, usb, sizeof usb);
    assert(strstr(usb, "operation_busy"));
    assert(task_calls == 0U && register_calls == 0U && config_commit_calls == 0U &&
           !s_product_active && !s_ota_active && !s_trial_active);
    poll_frp_restart(1099U);
    assert(restart_calls == 0U);
    fake_frp_response_pending = true;
    poll_frp_restart(1100U);
    poll_frp_restart(2999U);
    assert(restart_calls == 0U);
    poll_frp_restart(3000U);
    poll_frp_restart(3001U);
    assert(restart_calls == 1U && !s_frp_restart_pending);

    reset_case();
    expect_frp_restart(request, 202, "running", NULL);
    poll_frp_restart(1100U);
    assert(restart_calls == 1U);
    /* Old target boot cannot execute after boot identity changes. */
    strcpy(s_boot_id, other);
    expect_frp_restart(request, 409, "failed", "wrong_boot");
    assert(restart_calls == 1U && !s_frp_restart_pending);

    reset_case();
    handle_mqtt_command((const uint8_t *)request, strlen(request), NULL);
    expect_reply("running", NULL);
    assert(restart_calls == 0U && s_guard.count == 1U && s_mqtt_restart_pending);
    expect_frp_restart(request, 202, "running", NULL);
    assert(restart_calls == 0U && !s_frp_restart_pending && s_mqtt_restart_pending);
    mqtt_restart_acknowledged = true;
    poll_mqtt_restart(1100U);
    assert(restart_calls == 1U && !s_mqtt_restart_pending && mqtt_restart_result_calls == 1U);
    restart_request(second, 1U, device, boot, 30000U);
    expect_frp_restart(second, 409, "failed", "request_conflict");

    for (unsigned fault = 0; fault < 6U; ++fault) {
        reset_case();
        const char *expected = NULL;
        switch (fault) {
            case 0: s_ota_active = true; expected = "ota_in_progress"; break;
            case 1: s_product_active = true; expected = "operation_busy"; break;
            case 2: s_ota_boot_uncertain = true; expected = "ota_boot_state_unknown"; break;
            case 3: s_trial_active = true; expected = "configuration_busy"; break;
            case 4: s_config_uncertain = true; expected = "storage_uncertain"; break;
            case 5: esp_base_control_state_set_ota_pending(&s_control_state, true);
                    expected = "ota_verification_pending"; break;
        }
        expect_frp_restart(request, 409, "failed", expected);
        expect_frp_restart(request, 409, "failed", expected);
        assert(s_guard.count == 1U && !s_frp_restart_pending && restart_calls == 0U);
    }
    reset_case();
    restart_request(second, 2U, other, boot, 31000U);
    expect_frp_restart(second, 409, "failed", "wrong_device");
    restart_request(second, 2U, device, other, 31000U);
    expect_frp_restart(second, 409, "failed", "wrong_boot");
    restart_request(second, 2U, device, boot, 1000U);
    expect_frp_restart(second, 409, "expired", "expired");
    restart_request(second, 2U, device, boot, 31001U);
    expect_frp_restart(second, 409, "failed", "invalid_deadline");
    assert(s_guard.count == 0U && !s_frp_restart_pending);
    expect_frp_restart("{}", 400, "failed", "invalid_request");
    /* Fixed endpoint and JSON command must agree before admission. */
    char response[768]; size_t length = 0;
    assert(handle_frp_management(ESP_BASE_FRP_MANAGEMENT_STATUS,
        (const uint8_t *)request, strlen(request), response, sizeof response,
        &length, NULL) == 400);
    assert(strstr(response, "invalid_request") && s_guard.count == 0U);
    s_guard.count = EBASE_REQUEST_SLOTS;
    expect_frp_restart(request, 409, "failed", "capacity_exceeded");
    assert(!s_frp_restart_pending);
    reset_case();
    length = 0;
    assert(handle_frp_management(ESP_BASE_FRP_MANAGEMENT_RESTART,
        (const uint8_t *)request, strlen(request), response, 16U, &length, NULL) == 500);
    assert(!s_frp_restart_pending && length == 0U && s_guard.count == 1U);
    expect_frp_restart(request, 409, "failed", "resource_failure");
}

int main(void)
{
    check_mqtt_restart();
    check_frp_restart();
    check_firmware_package_health();
    check_package_ota_worker();
    reset_case();
    allow_config_load = true;
    uint32_t config_revision = 0U;
    assert(esp_base_protocol_load_config(&config_revision, &owner) == ESP_OK &&
           config_revision == 7U && config_load_calls == 1U && s_config_loaded);
    assert(atomic_load(&owner.active_token) == 0U);
    esp_base_storage_claim_t held_flash = {0};
    assert(esp_base_storage_claim(&owner, &held_flash));
    assert(esp_base_protocol_load_config(&config_revision, &owner) == ESP_ERR_TIMEOUT &&
           config_load_calls == 1U && !s_config_loaded);
    assert(esp_base_storage_claim_active(&held_flash));
    assert(esp_base_storage_release(&held_flash));
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
           strstr(latest_reply, "\"firmware_sha256\":\"f1f1") &&
           strstr(latest_reply, "\"runtime_guest_abi_version\":2") &&
           strstr(latest_reply, "\"package_guest_abi_version\":null") &&
           strstr(latest_reply, "\"package_data_schema_version\":null") &&
           strstr(latest_reply, "\"active_product\":null") &&
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
           strstr(latest_reply, "\"package_guest_abi_version\":2") &&
           strstr(latest_reply, "\"package_data_schema_version\":1") &&
           atomic_load(&owner.active_token) == 0U);
    for (active_fixture_mode = 1U; active_fixture_mode <= 2U; ++active_fixture_mode) {
        s_reply_mqtt = true;
        handle_line("product-status-1", strlen("product-status-1"), NULL);
        s_reply_mqtt = false;
        expect_reply("succeeded", NULL);
        assert(strlen(latest_reply) > 4096U && strlen(latest_reply) <= 5120U &&
               strstr(latest_reply, "\"product_id\":\"counter\"") &&
               strstr(latest_reply, active_fixture_mode == 2U ? "\"is_trial\":true" : "\"is_trial\":false"));
    }
    active_fixture_mode = 0U;
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
           strstr(latest_reported, "\"last_completed_event_sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\"") &&
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
    feed_serial((const unsigned char *)"config-20", 9U);
    assert(s_reader != NULL && s_reader->data != NULL);
    expire_serial_input(2099U, 100U);
    assert(s_reader != NULL && !s_serial_discard);
    expire_serial_input(2100U, 100U);
    assert(s_reader == NULL && s_serial_discard);
    feed_serial((const unsigned char *)"discarded-tail\nconfig-24\n", 25U);
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
    esp_base_storage_claim_t held_config_flash = {0};
    assert(esp_base_storage_claim(&owner, &held_config_flash));
    poll_configuration(1001U);
    assert(s_trial_active && s_candidate != NULL && config_commit_calls == 0U &&
           esp_base_storage_claim_active(&held_config_flash));
    assert(esp_base_storage_release(&held_config_flash));
    poll_configuration(1002U);
    assert(!s_trial_active && s_candidate == NULL && config_commit_calls == 1U &&
           s_context.config.revision == 8U && wifi_apply_calls == 1U);
#if defined(CONFIG_IDF_TARGET_ESP32)
    /* Header, transferred config and commit workspace preserve ESP32 IRAM
     * ownership without allocating a duplicate candidate. */
    assert(iram_work_allocations == 3U);
#endif

    for (unsigned failed_reload = 0U; failed_reload < 2U; ++failed_reload) {
        reset_case();
        s_context.config.revision = 7U;
        s_context.config.frp.configured = true;
        strcpy(s_context.config.frp.server_hostname, "admitted-frp.example");
        s_context.frp_flash_store = &frp_store;
        const esp_base_remote_config_t admitted = s_context.config;
        config_set(23U, false, usb_reply, sizeof usb_reply);
        assert(s_trial_active && s_candidate != NULL);
        config_commit_result = ESP_BASE_CONFIG_UNCERTAIN;
        allow_config_load = true;
        config_load_output = (esp_base_remote_config_t){.revision = 8U};
        config_load_output.frp.configured = true;
        strcpy(config_load_output.frp.server_hostname, "reloaded-frp.example");
        config_load_result = failed_reload ? ESP_ERR_INVALID_STATE : ESP_OK;
        poll_configuration(1002U);
        assert(!s_trial_active && s_candidate == NULL && s_config_uncertain &&
               config_commit_calls == 1U && config_load_calls == 1U);
        if (failed_reload) {
            assert(memcmp(&s_context.config, &admitted, sizeof admitted) == 0);
        } else {
            assert(memcmp(&s_context.config, &config_load_output,
                          sizeof config_load_output) == 0);
        }
    }

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
    check_frp_status("status-1", 200, NULL, 1000U);
    fake_free_heap = 500;
    fake_now_ms = 2000U;
    check_frp_status("status-1", 200, NULL, 1000U);
    check_frp_status("same-id-wrong-device", 409, "wrong_device", 0U);
    check_frp_status("status-2", 200, NULL, 500U);
    check_frp_status("wrong-device", 409, "wrong_device", 0U);
    check_frp_status("invalid", 400, "invalid_request", 0U);
    for (unsigned i = 3; i <= FRP_STATUS_REPLAY_SLOTS; ++i) {
        char request[16];
        snprintf(request, sizeof request, "status-%u", i);
        check_frp_status(request, 200, NULL, 500U);
    }
    check_frp_status("status-9", 400, "capacity_exceeded", 0U);
    fake_now_ms = 31000U; /* First snapshot's server-owned TTL has expired. */
    check_frp_status("status-1", 200, NULL, 500U);
    fake_now_ms = 32000U;
    check_frp_status("status-9", 200, NULL, 500U);
    assert(config_commit_calls == 0U && register_calls == 0U && task_calls == 0U &&
           atomic_load(&owner.active_token) == 0U);
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
    start(85);
    expect_reply("failed", "product_ota_unavailable");
    assert(register_calls == 0 && task_calls == 0 && retire_calls == 0 &&
           product_retire_calls == 0 && atomic_load(&owner.active_token) == 0);

    reset_case();
    start(86);
    expect_reply("failed", "product_ota_unavailable");
    assert(register_calls == 0 && task_calls == 0 && retire_calls == 0 &&
           product_retire_calls == 0 && atomic_load(&owner.active_token) == 0);

    reset_case();
    start(87);
    expect_reply("failed", "invalid_request");
    assert(register_calls == 0 && task_calls == 0 && retire_calls == 0 &&
           product_retire_calls == 0 && atomic_load(&owner.active_token) == 0);

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
    loaded_package_mode = ESP_BASE_OTA_PACKAGE_REUSE;
    start(20);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(load_receipt_calls == 1 && retire_calls == 0 &&
           product_retire_calls == 0 && prepare_calls == 0 &&
           stage_calls == 0 && select_calls == 0 && failure_record_calls == 0);

    reset_case();
    product_retire_result = ESP_BASE_CONTAINER_RETIRE_UNCERTAIN;
    start(18);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(retire_calls == 1 && product_retire_calls == 1 && prepare_calls == 0);
    check_product_run_guard();
    check_product_uninstall_path();
    check_product_package_guard();
    check_product_trial_health();
    check_product_package_preboot_recovery();
    const uint8_t business_bytes[] = {1U, 2U, 3U};
    ebase_mqtt_event_view_t business_event = {
        .event_sequence = 1U, .event = business_bytes,
        .event_size_bytes = sizeof business_bytes,
    };
    memset(business_event.package_sha256, 0x11, 32);
    uint8_t expected_event_digest[32];
    size_t digest_size = 0U;
    assert(psa_hash_compute(PSA_ALG_SHA_256, business_bytes,
        sizeof business_bytes, expected_event_digest,
        sizeof expected_event_digest, &digest_size) == PSA_SUCCESS && digest_size == 32U);
    offered_event_result = ESP_BASE_CONTAINER_EVENT_ACCEPTED;
    assert(handle_mqtt_event(&business_event, NULL) && offered_event_calls == 1U &&
           !memcmp(offered_event_digest, expected_event_digest, 32));
    offered_event_result = ESP_BASE_CONTAINER_EVENT_FULL;
    assert(!handle_mqtt_event(&business_event, NULL) && offered_event_calls == 2U);
    esp_base_ota_receipt_view_t ota_view = {
        .state = ESP_BASE_OTA_OPERATION_RUNNING,
        .image_size_bytes = 4096U,
        .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .package_mode = ESP_BASE_OTA_PACKAGE_REUSE,
    };
    strcpy(ota_view.operation_id, "44444444-4444-4444-8444-000000000085");
    memset(ota_view.sha256, 0x5a, 32);
    memset(ota_view.package_sha256, 0x7b, 32);
    s_reply_mqtt = true;
    reply_ota_result("11111111-1111-4111-8111-000000000085", &ota_view);
    assert(strstr(latest_reply, "\"package_mode\":\"reuse\"") != NULL &&
           strstr(latest_reply, "\"package_sha256\":\"7b7b7b7b") != NULL);
    ota_view.package_mode = ESP_BASE_OTA_NO_PACKAGE;
    memset(ota_view.package_sha256, 0, 32);
    reply_ota_result("11111111-1111-4111-8111-000000000086", &ota_view);
    assert(strstr(latest_reply, "\"package_mode\":\"no_package\"") != NULL &&
           strstr(latest_reply, "\"package_sha256\":null") != NULL);
    s_reply_mqtt = false;
    puts("  protocol_ota_owner passed (OTA owner faults; product ledger/uninstall/recovery; USB FRP storage gate; MQTT write rejection)");
    return 0;
}

const char *ebase_parse_command(const char *line, size_t length,
                                ebase_command_t *out, ebase_command_alloc_t allocate)
{
    if (line == NULL) {
        memset(out, 0, sizeof *out);
        return "invalid_request";
    }
    if (length && line[0] == '{') return ebase_parse_command_real(line, length, out, allocate);
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
    const size_t payload_size = configure ? sizeof *out->config :
        query || product_query ? ESP_BASE_OTA_OPERATION_ID_BYTES :
        product_status_query ? 0U :
        product_package_command ? sizeof *out->product_package :
        product_uninstall_command ? sizeof *out->product_uninstall : sizeof *out->ota;
    if (payload_size) {
        out->payload = allocate(payload_size);
        if (!out->payload) return "resource_failure";
        out->payload_size_bytes = payload_size;
        memset(out->payload, 0, payload_size);
    }
    snprintf(out->request.request_id, sizeof out->request.request_id,
             "11111111-1111-4111-8111-%012u",
             product_package_command && (number == 41U || number == 46U) ? 40U : number);
    strcpy(out->request.device_id, "22222222-2222-4222-8222-222222222222");
    strcpy(out->request.boot_id, "33333333-3333-4333-8333-333333333333");
    out->request.expires_at_ms = 10000;
    if (product_status_query) return NULL;
    if (configure) {
        out->config->revision = 7U;
        out->config->frp.configured = true;
        out->config->wifi.configured = true;
        return NULL;
    }
    if (query || product_query) {
        snprintf(out->operation_id, ESP_BASE_OTA_OPERATION_ID_BYTES,
                 "44444444-4444-4444-8444-%012u", number);
        return NULL;
    }
    if (product_uninstall_command) {
        strcpy(out->product_uninstall->operation_id,
               "44444444-4444-4444-8444-000000000001");
        out->product_uninstall->operation_sequence = number == 34U ? 2U : 1U;
        out->product_uninstall->expected_container_sequence = 6U;
        memset(out->product_uninstall->package_sha256, 0x7b, 32);
        return NULL;
    }
    if (product_package_command) {
        ebase_product_package_request_t *package = out->product_package;
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
        memset(package->trial_event_sha256, 0x22, 32);
        if (number == 46U) memset(package->trial_event_sha256, 0x23, 32);
        strcpy(package->package_url, number == 42U ?
            "https://user@packages.example.test/a.pkg" :
             number == 41U || number == 43U ?
                "https://packages.example.test/b.pkg" :
            "https://packages.example.test/a.pkg");
        return NULL;
    }
    snprintf(out->ota->operation_id, sizeof out->ota->operation_id,
             "44444444-4444-4444-8444-%012u", number);
    strcpy(out->ota->image_url, "https://example.invalid/signed.bin");
    out->ota->image_size_bytes = 4096;
    memset(out->ota->sha256, 0x5a, sizeof out->ota->sha256);
    if (number == 85U || number == 86U || number == 87U) {
        out->ota->package_mode = number == 85U ? ESP_BASE_OTA_PACKAGE_REUSE :
                                ESP_BASE_OTA_PACKAGE_WRITE;
        out->ota->package_size_bytes = 10240U;
        out->ota->guest_abi_version = 2U;
        out->ota->data_schema_version = 1U;
        memset(out->ota->package_sha256, 0x7b, 32);
        memset(out->ota->trial_event_sha256, 0x22, 32);
        if (number != 85U)
            strcpy(out->ota->package_url, number == 87U ?
                   "http://packages.example.test/a.pkg" :
                   "https://packages.example.test/a.pkg");
    }
    return NULL;
}

static ebase_ledger_io_result_t read_product_ledger(
    void *context, uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    (void)bytes;
    assert(context == &owner);
    if (product_ledger_read_busy) return EBASE_LEDGER_IO_BUSY;
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
    if (length && json[0] == '{') return ebase_parse_frp_status_real(json, length, out);
    memset(out, 0, sizeof *out);
    if (!strcmp(json, "invalid")) return "invalid_request";
    strcpy(out->request_id, "11111111-1111-4111-8111-111111111111");
    if (!strncmp(json, "status-", 7)) out->request_id[35] = json[7];
    strcpy(out->device_id, (!strcmp(json, "wrong-device") || !strcmp(json, "same-id-wrong-device")) ?
        "99999999-9999-4999-8999-999999999999" : "22222222-2222-4222-8222-222222222222");
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
bool esp_base_mqtt_owner_ready(void) { return mqtt_ready; }
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
int64_t esp_timer_get_time(void) { return (int64_t)fake_now_ms * 1000; }

esp_err_t esp_base_mqtt_owner_configure(const ebase_mqtt_config_t *config,
    const char *device_id, const char *boot_id, emqtt_config_t *scratch)
{
    assert(config == &s_context.config.mqtt &&
           !strcmp(device_id, s_context.device_id) && !strcmp(boot_id, s_boot_id) && scratch);
    ++mqtt_configures;
    return ESP_OK;
}
esp_err_t esp_base_mqtt_owner_revoke(void)
{
    ++mqtt_revokes;
    return mqtt_revoke_result;
}
void esp_base_mqtt_owner_poll(uint64_t now_ms, bool network, bool trusted,
    ebase_mqtt_command_handler_t commands, ebase_mqtt_event_handler_t events, void *context)
{
    assert(now_ms == fake_now_ms && network == network_ready &&
           trusted == trusted_time_ready && commands && events && context == NULL);
    ++mqtt_polls;
}
esp_err_t esp_base_frp_owner_configure(const ebase_frp_config_t *config,
    const char *device_id, const efrp_aead_flash_store_t *store)
{
    assert(config == &s_context.config.frp && !strcmp(device_id, s_context.device_id) &&
           store == s_context.frp_flash_store);
    ++frp_configures;
    return ESP_OK;
}
void esp_base_frp_owner_poll(uint64_t now_ms, bool network, bool trusted, bool ready)
{
    assert(now_ms == fake_now_ms && network == network_ready &&
           trusted == trusted_time_ready && ready);
    ++frp_polls;
}
#ifndef ESP_BASE_TEST_REAL_FRP_LISTENER
void esp_base_frp_management_listener_configure(const ebase_frp_config_t *config)
{
    assert(config == NULL || config == &s_context.config.frp);
    ++listener_configures;
}
void esp_base_frp_management_listener_poll(uint64_t now_ms,
    esp_base_frp_management_handler_t handler, void *context)
{
    assert(now_ms == fake_now_ms && handler && context == NULL);
    ++listener_polls;
}
bool esp_base_frp_management_listener_ready(void) { return true; }
bool esp_base_frp_management_listener_response_pending(void) { return fake_frp_response_pending; }
#endif
uint64_t esp_base_mqtt_owner_event_sequence(void) { return 3; }
esp_base_container_event_observation_result_t
esp_base_container_product_event_observation(
    esp_base_container_event_observation_t *out)
{
    *out = (esp_base_container_event_observation_t){0};
    if (fake_event_observation_state == ESP_BASE_CONTAINER_EVENT_OBSERVED) {
        memset(out->package_sha256, fake_observed_package_byte, 32);
        memset(out->event_sha256, fake_observed_event_byte, 32);
        out->event_sequence = 2U;
        out->guest_result = fake_event_guest_result;
        out->runtime_ok = fake_event_runtime_ok;
    }
    return fake_event_observation_state;
}
bool esp_base_container_product_trial_event_snapshot(
    esp_base_container_trial_event_snapshot_t *out)
{
    *out = (esp_base_container_trial_event_snapshot_t){0};
    if (!fake_trial_snapshot_ready) return false;
    out->representative_event_sequence = fake_trial_representative_sequence;
    out->failure_count = fake_trial_failure_count;
    memset(out->package_sha256, fake_observed_package_byte, 32);
    return true;
}
bool esp_base_mqtt_owner_result(const char *json, size_t length)
{
    assert(length < sizeof latest_reply);
    memcpy(latest_reply, json, length);
    latest_reply[length] = '\0';
    return true;
}
bool esp_base_mqtt_owner_restart_result(const char *json, size_t length)
{
    if (!mqtt_restart_enqueue_ok) return false;
    ++mqtt_restart_result_calls;
    return esp_base_mqtt_owner_result(json, length);
}
bool esp_base_mqtt_owner_restart_result_acknowledged(void)
{
    return mqtt_restart_acknowledged;
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
        .package_mode = loaded_package_mode,
    };
    memcpy(receipt->operation_id, s_ota_request.operation_id,
           sizeof receipt->operation_id);
    memcpy(receipt->source_sha256,
           (uint8_t[32]){0xa0}, sizeof receipt->source_sha256);
    memcpy(receipt->candidate_sha256, s_ota_request.sha256,
           sizeof receipt->candidate_sha256);
    memcpy(receipt->package_sha256, s_ota_request.package_sha256, 32);
    memcpy(receipt->trial_event_sha256, s_ota_request.trial_event_sha256, 32);
    receipt->package_size_bytes = s_ota_request.package_size_bytes;
    receipt->guest_abi_version = s_ota_request.guest_abi_version;
    receipt->data_schema_version = s_ota_request.data_schema_version;
    if (ota_package_receipt_changed) receipt->trial_event_sha256[0] ^= 1U;
    receipt->source_package_present = ota_source_package_present;
    if (ota_source_package_present) {
        memset(receipt->source_package_sha256,
               s_ota_request.package_mode == ESP_BASE_OTA_PACKAGE_REUSE ? 0x7b : 0x7a, 32);
        receipt->source_package_size_bytes = 10240U;
        receipt->source_guest_abi_version = 2U;
        receipt->source_data_schema_version = 1U;
    }
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
    const esp_base_ota_receipt_recovery_t *receipt)
{
    assert(esp_base_storage_claim_active(claim) && prepared != NULL &&
           receipt != NULL && receipt->operation_id[0] == '4' &&
           (!receipt->source_package_present || package_stop_calls == 1U));
    ++stage_calls;
    return stage_result;
}

bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim, const esp_base_ota_request_t *request,
    esp_base_ota_receipt_snapshot_t *snapshot)
{
    assert(esp_base_storage_claim_active(claim) &&
           request && snapshot);
    ++snapshot_calls;
    *snapshot = (esp_base_ota_receipt_snapshot_t){
        .container_enabled = product_configured,
        .container_sequence = 7,
    };
    snapshot->source_sha256[0] = 0xa0;
    snapshot->source_package_present = ota_source_package_present;
    if (ota_source_package_present) {
        memset(snapshot->source_package_sha256,
               request->package_mode == ESP_BASE_OTA_PACKAGE_REUSE ? 0x7b : 0x7a, 32);
        snapshot->source_package_size_bytes = 10240U;
        snapshot->source_guest_abi_version = 2U;
        snapshot->source_data_schema_version = 1U;
    }
    if (ota_source_snapshot_changed && snapshot_calls > 1U)
        ++snapshot->container_sequence;
    return snapshot_ok;
}

esp_base_container_stage_result_t esp_base_container_product_write_staged_firmware_package(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const esp_base_ota_receipt_recovery_t *receipt,
    econtainer_slot_source_fn source_fn, void *source_context)
{
    assert(esp_base_storage_claim_active(claim) && prepared && receipt &&
           receipt->package_mode == ESP_BASE_OTA_PACKAGE_WRITE &&
           stage_calls == 1U && stage_result == ESP_BASE_CONTAINER_STAGE_WRITING &&
           (!receipt->source_package_present || package_stop_calls == 1U));
    uint8_t byte = 0;
    assert(source_fn && source_context && source_fn(source_context, 0U, &byte, 1U) && byte == 0x7bU);
    ++ota_package_write_calls;
    return ota_package_write_result;
}

esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt)
{
    assert(esp_base_storage_claim_active(claim) &&
           receipt && receipt->container_enabled == product_configured &&
           receipt->container_sequence == 7U && receipt->source_sha256[0] == 0xa0 &&
           receipt->package_mode == s_ota_request.package_mode);
    ++product_retire_calls;
    return product_retire_result;
}

bool esp_base_container_product_ota_ready(esp_base_ota_package_mode_t mode)
{
    ++ota_ready_calls;
    if (mode != ESP_BASE_OTA_NO_PACKAGE && (!product_configured ||
        (mode == ESP_BASE_OTA_PACKAGE_REUSE && !ota_source_package_present))) return false;
    return product_ota_ready && (ota_ready_calls == 1 || ota_ready_after_first);
}

bool esp_base_container_product_configured(void)
{
    return product_configured;
}

esp_base_container_run_result_t esp_base_container_product_set_running(
    const esp_base_storage_claim_t *claim, bool running, const char boot_id[37],
    uint32_t expected_sequence, const uint8_t package_sha256[32])
{
    assert(esp_base_storage_claim_active(claim) &&
        !strcmp(boot_id, s_boot_id) && expected_sequence == binding_sequence &&
        package_sha256[0] == 0x7b);
    ++product_run_calls;
    product_run_started = running;
    return product_run_result;
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
    memset(out->firmware_sha256, 0xf1, 32);
    out->runtime_guest_abi_version = 2U;
    out->package_guest_abi_version = binding_package_present ? 2U : 0U;
    out->package_data_schema_version = binding_package_present ? 1U : 0U;
    if (binding_package_present) memset(out->package_sha256, 0x7b, 32);
    return binding_result;
}

esp_base_container_binding_result_t esp_base_container_product_status_snapshot(
    const esp_base_storage_claim_t *claim,
    esp_base_container_binding_snapshot_t *binding,
    esp_base_container_active_product_t *active)
{
    *active = (esp_base_container_active_product_t){0};
    const esp_base_container_binding_result_t result = esp_base_container_product_binding_snapshot(claim, binding);
    if (result == ESP_BASE_CONTAINER_BINDING_OK && active_fixture_mode != 0U) {
        active->present = true;
        active->product_id = "counter";
        active->product_id_size_bytes = sizeof("counter") - 1U;
        active->product_version_size_bytes = 3900U;
        active->product_version = malloc(3901U);
        assert(active->product_version != NULL);
        memset(active->product_version, 'v', 3900U);
        active->product_version[3900U] = '\0';
        memcpy(active->package_sha256, binding->package_sha256, 32);
        active->guest_abi_version = 2U;
        active->data_schema_version = 1U;
        active->is_trial = active_fixture_mode == 2U;
        if (active->is_trial) {
            strcpy(active->operation_id, "44444444-4444-4444-8444-000000000002");
            memset(active->package_sha256, 0xab, 32);
        }
    }
    return result;
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
    if (s_ota_active) assert(prepare_calls == 1U && stage_calls == 0U);
    ++package_stop_calls;
    return package_stop_ok;
}

esp_base_container_boot_result_t esp_base_container_product_start_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t prepared_sequence,
    const char operation_id[37], const char boot_id[37],
    const uint8_t trial_event_sha256[32])
{
    assert(esp_base_storage_claim_active(claim) && prepared_sequence == 8U &&
           operation_id != NULL && !strcmp(boot_id, s_boot_id) &&
           trial_event_sha256[0] == 0x22U);
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
bool esp_base_container_product_trial_quiescent(void)
{
    return package_event_accepting && fake_trial_quiescent;
}

esp_base_container_trial_confirm_result_t esp_base_container_product_confirm_package_trial(
    const esp_base_storage_claim_t *claim, uint32_t trial_sequence,
    const char operation_id[37], uint64_t verified_event_sequence,
    const uint8_t verified_event_sha256[32], uint64_t verified_failure_count,
    uint32_t *confirmed_sequence)
{
    assert(esp_base_storage_claim_active(claim) && trial_sequence == 9U &&
           !strcmp(operation_id, s_product_operation_id) &&
           verified_event_sequence == 2U && verified_event_sha256[0] == 0x22U &&
           verified_failure_count == fake_trial_failure_count &&
           confirmed_sequence != NULL);
    ++package_confirm_calls;
    if (package_confirm_not_started)
        return ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    if (package_confirm_ok) {
        *confirmed_sequence = 11U;
        binding_sequence = 11U;
        binding_package_present = true;
    }
    return package_confirm_ok ? ESP_BASE_CONTAINER_CONFIRM_CONFIRMED :
        ESP_BASE_CONTAINER_CONFIRM_UNCERTAIN;
}

esp_base_container_event_result_t esp_base_container_product_offer_event(
    const uint8_t package_sha256[32], uint64_t event_sequence,
    const uint8_t event_sha256[32], const uint8_t *event, size_t size_bytes)
{
    assert(package_sha256[0] == 0x11 && event_sequence == 1U &&
           event != NULL && size_bytes == 3U &&
           event[0] == 1U && event[1] == 2U && event[2] == 3U);
    memcpy(offered_event_digest, event_sha256, sizeof offered_event_digest);
    ++offered_event_calls;
    return offered_event_result;
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
    esp_base_storage_claim_t competing = {0};
    assert(s_context.flash_io_owner != NULL &&
           !esp_base_storage_claim(s_context.flash_io_owner, &competing));
    ++config_commit_calls;
    if (config_commit_result != ESP_OK) return config_commit_result;
    *committed = *candidate;
    committed->revision = expected_revision + 1U;
    return ESP_OK;
}
esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    assert(allow_config_load && config != NULL);
    esp_base_storage_claim_t competing = {0};
    assert(!esp_base_storage_claim(&owner, &competing));
    ++config_load_calls;
    *config = config_load_output;
    return config_load_result;
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
    assert(ticks == pdMS_TO_TICKS(100));
}
