// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the real command branch and its asynchronous completion branch. */
#include "../components/device_protocol/esp_base_protocol.c"

static esp_base_storage_owner_t owner;
static uint8_t product_bytes[EBASE_PRODUCT_LEDGER_BYTES];
static bool product_present;
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
    memset(product_bytes, 0, sizeof product_bytes);
    strcpy(s_boot_id, "33333333-3333-4333-8333-333333333333");
    memset(&s_guard, 0, sizeof s_guard);
    memset(s_outcomes, 0, sizeof s_outcomes);
    memset(s_frp_status_seen, 0, sizeof s_frp_status_seen);
    memset(&s_ota_storage_claim, 0, sizeof s_ota_storage_claim);
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
    assert(strstr(latest_reply, field));
    if (error) {
        (void)snprintf(field, sizeof field, "\"error_code\":\"%s\"", error);
        assert(strstr(latest_reply, field));
    }
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
    s_reply_mqtt = true;
    handle_line("product-1", 9, NULL);
    s_reply_mqtt = false;
    expect_reply("unknown", "product_operation_not_found");
    const ebase_product_ledger_io_t product_io = ebase_product_ledger_nvs_io(&owner);
    ebase_product_ledger_t product_ledger;
    assert(ebase_product_ledger_open(&product_ledger, &product_io) == EBASE_LEDGER_UNINITIALIZED);
    assert(ebase_product_ledger_initialize_empty(&product_ledger, &product_io) == EBASE_LEDGER_OK);
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
           strstr(latest_reply, "\"pending_operation_id\":null"));
    product_intent.sequence = 2U;
    strcpy(product_intent.operation_id, "44444444-4444-4444-8444-000000000002");
    assert(ebase_product_ledger_begin(&product_ledger, &product_io, &product_intent) == EBASE_LEDGER_OK);
    s_reply_mqtt = true;
    handle_line("product-status-1", strlen("product-status-1"), NULL);
    s_reply_mqtt = false;
    expect_reply("succeeded", NULL);
    assert(strstr(latest_reply, "\"next_operation_sequence\":3") &&
           strstr(latest_reply, "\"pending_operation_id\":\"44444444-4444-4444-8444-000000000002\""));
    s_reply_mqtt = true;
    reply("11111111-1111-4111-8111-111111111111", "succeeded", NULL, NULL);
    s_reply_mqtt = false;
    char copied_result[sizeof latest_reply];
    strcpy(copied_result, latest_reply);
    reported();
    assert(reported_calls == 1 && strstr(latest_reported, "\"frp_state\":\"stopped\"") &&
           !strcmp(latest_reply, copied_result));
    char copied_reported[sizeof latest_reported];
    strcpy(copied_reported, latest_reported);
    s_reply_mqtt = true;
    reply("11111111-1111-4111-8111-111111111112", "failed", "invalid_request", NULL);
    s_reply_mqtt = false;
    assert(!strcmp(latest_reported, copied_reported));
    char oversized_state[512];
    memset(oversized_state, 'x', sizeof oversized_state - 1);
    oversized_state[sizeof oversized_state - 1] = '\0';
    fake_frp_state = oversized_state;
    reported();
    assert(reported_calls == 1 && !strcmp(latest_reported, copied_reported));

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
    puts("  protocol_ota_owner passed (OTA owner faults; product result query; USB FRP storage gate; MQTT write rejection)");
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
    assert(configure || query || product_query || product_status_query ||
           (length > 6U && sscanf(line, "start-%u", &number) == 1));
    memset(out, 0, sizeof *out);
    out->kind = configure ? EBASE_CONFIG_SET :
                query ? EBASE_OTA_RESULT :
                product_query ? EBASE_PRODUCT_RESULT :
                product_status_query ? EBASE_PRODUCT_STATUS : EBASE_OTA_START;
    snprintf(out->request.request_id, sizeof out->request.request_id,
             "11111111-1111-4111-8111-%012u", number);
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
bool esp_base_wifi_ready(void) { return true; }
bool esp_base_time_ready(void) { return true; }
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
