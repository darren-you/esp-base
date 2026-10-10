// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the real command branch and its asynchronous completion branch. */
static void protocol_test_free(void *data);
#define free protocol_test_free
#include "../components/device_protocol/esp_base_protocol.c"
#undef free

void esp_base_capacity_poll(const char *boot_id, uint64_t now_ms)
{ (void)boot_id; (void)now_ms; }
void esp_base_capacity_before_reset(const char *boot_id, uint64_t now_ms)
{ (void)boot_id; (void)now_ms; }

static unsigned ota_request_releases;
static void *last_decoded_ota_request;

static void protocol_test_free(void *data)
{
    if (data != NULL && data == s_ota_request) {
        const unsigned char *bytes = data;
        for (size_t index = 0; index < sizeof *s_ota_request; ++index)
            assert(bytes[index] == 0U);
        ++ota_request_releases;
    }
    free(data);
}

static esp_base_storage_owner_t owner;
static bool network_ready, trusted_time_ready, mqtt_ready, signed_enabled;
static bool snapshot_ok, worker_created, defer_ota_worker, receipt_mismatch;
static esp_base_ota_receipt_result_t register_result, failure_record_result, load_result, query_result;
static esp_base_ota_operation_state_t query_state;
static eota_result_t prepare_result, select_result, retire_result, validate_result;
static eota_result_t second_retire_result;
static bool upload_arm_ok, upload_connected, upload_cancel_after_prepare;
static unsigned upload_arm_calls, upload_finish_calls, stream_prepare_calls;
static int upload_finish_status;
static unsigned failure_record_calls_at_upload_finish;
static char upload_finish_reply[512];
static unsigned register_calls, failure_record_calls, task_calls, prepare_calls, select_calls;
static unsigned snapshot_calls, load_receipt_calls, retire_calls, validate_calls, query_calls, restart_calls;
static unsigned reported_calls, wifi_apply_calls, config_commit_calls, config_load_calls;
static unsigned mqtt_configures, mqtt_polls, mqtt_revokes, frp_configures, frp_polls;
static unsigned listener_configures, listener_polls, mqtt_restart_result_calls;
static bool allow_config_load, fake_frp_response_pending, mqtt_restart_enqueue_ok, mqtt_restart_acknowledged;
static esp_err_t config_commit_result, config_load_result;
static esp_base_remote_config_t config_load_output;
static esp_base_ota_request_t registered_request;
static uint8_t registered_source_sha256[32];
static const char *fake_frp_state;
static uint32_t fake_free_heap;
static uint64_t fake_now_ms;
static char latest_reply[5121], latest_reported[768];
#if defined(CONFIG_IDF_TARGET_ESP32)
static unsigned iram_work_allocations;
#endif

const char *ebase_parse_command_real(const char *, size_t, ebase_command_t *, ebase_command_alloc_t);
const char *ebase_parse_frp_status_real(const char *, size_t, ebase_request_t *);
const char *ebase_parse_command(const char *line, size_t length,
                              ebase_command_t *out, ebase_command_alloc_t allocate)
{
    const char *error = ebase_parse_command_real(line, length, out, allocate);
    if (!error && out->kind == EBASE_OTA_START) last_decoded_ota_request = out->ota;
    return error;
}
const char *ebase_parse_frp_status(const char *line, size_t length, ebase_request_t *out)
{ return ebase_parse_frp_status_real(line, length, out); }

static void reset_case(void)
{
    clear_ota_request();
    clear_candidate();
    ebase_line_release(s_reader); free(s_reader); s_reader = NULL;
    s_serial_discard = false;
    esp_base_storage_owner_init(&owner);
    s_context = (protocol_state_t){.device_id = "22222222-2222-4222-8222-222222222222",
        .firmware_version = "test", .chip_model = "esp32c3", .flash_size_bytes = 0x400000,
        .storage_owner = &owner, .flash_io_owner = &owner, .config.revision = 7};
    strcpy(s_boot_id, "33333333-3333-4333-8333-333333333333");
    memset(&s_guard, 0, sizeof s_guard); memset(s_outcomes, 0, sizeof s_outcomes);
    memset(s_frp_status_seen, 0, sizeof s_frp_status_seen);
    s_ota_storage_claim = (esp_base_storage_claim_t){0};
    s_config_uncertain = s_ota_boot_uncertain = s_ota_active = s_trial_active = false;
    s_started = s_config_loaded = false;
    s_mqtt_revision_set = s_frp_revision_set = false;
    s_frp_restart_pending = s_mqtt_restart_pending = false;
    s_reply_mqtt = false; s_parse_frp = false;
    s_frp_reply = NULL; s_frp_reply_length = s_frp_reply_capacity = 0;
    atomic_store(&s_ota_done, false); atomic_store(&s_ota_stage_uncertain, false);
    atomic_store(&s_ota_received, 0);
    esp_base_control_state_set_ota_pending(&s_control_state, false);
    esp_base_control_state_set_ota_download_active(&s_control_state, false);
    ebase_business_reset(&s_business);
    network_ready = trusted_time_ready = mqtt_ready = signed_enabled = true;
    snapshot_ok = worker_created = defer_ota_worker = true;
    receipt_mismatch = false; upload_arm_ok = upload_connected = true; upload_cancel_after_prepare = false;
    upload_arm_calls = upload_finish_calls = stream_prepare_calls = 0;
    upload_finish_status = 0; failure_record_calls_at_upload_finish = 0;
    upload_finish_reply[0] = '\0';
    register_result = failure_record_result = load_result = ESP_BASE_OTA_RECEIPT_OK;
    query_result = ESP_BASE_OTA_RECEIPT_NOT_FOUND; query_state = ESP_BASE_OTA_OPERATION_SUCCEEDED;
    prepare_result = select_result = retire_result = validate_result = second_retire_result = EOTA_UPDATE_OK;
    register_calls = failure_record_calls = task_calls = prepare_calls = select_calls = 0;
    snapshot_calls = load_receipt_calls = retire_calls = validate_calls = query_calls = restart_calls = 0;
    reported_calls = wifi_apply_calls = config_commit_calls = config_load_calls = 0;
    mqtt_configures = mqtt_polls = mqtt_revokes = frp_configures = frp_polls = 0;
    listener_configures = listener_polls = mqtt_restart_result_calls = 0;
    allow_config_load = false; fake_frp_response_pending = false;
    mqtt_restart_enqueue_ok = mqtt_restart_acknowledged = true;
    config_commit_result = config_load_result = ESP_OK;
    config_load_output = (esp_base_remote_config_t){.revision = 7};
    memset(&registered_request, 0, sizeof registered_request);
    memset(registered_source_sha256, 0x41, sizeof registered_source_sha256);
    fake_now_ms = 1000; fake_frp_state = "ready"; fake_free_heap = 16384;
    latest_reply[0] = latest_reported[0] = '\0';
    ota_request_releases = 0; last_decoded_ota_request = NULL;
#if defined(CONFIG_IDF_TARGET_ESP32)
    iram_work_allocations = 0;
#endif
}

static void expect_reply(const char *state, const char *error)
{
    char expected[128]; snprintf(expected, sizeof expected, "\"state\":\"%s\"", state);
    if (!strstr(latest_reply, expected)) fprintf(stderr, "expected %s; got %s\n", expected, latest_reply);
    assert(strstr(latest_reply, expected));
    snprintf(expected, sizeof expected, error ? "\"error_code\":\"%s\"" : "\"error_code\":null", error);
    if (!strstr(latest_reply, expected)) fprintf(stderr, "expected %s; got %s\n", expected, latest_reply);
    assert(strstr(latest_reply, expected));
}
static void send_json(const char *json)
{
    s_reply_mqtt = true; handle_line(json, strlen(json), NULL); s_reply_mqtt = false;
}
static void start(unsigned number, const char *device, const char *boot, uint64_t expires)
{
    char json[1600];
    snprintf(json, sizeof json,
        "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-%012u\","
        "\"device_id\":\"%s\",\"target_boot_id\":\"%s\",\"expires_at_uptime_ms\":%" PRIu64 ","
        "\"command\":\"ota.start\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-%012u\","
        "\"image_url\":\"https://example.test/base.bin\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
        "\"image_size_bytes\":4096,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}}",
        number, device, boot, expires, number);
    send_json(json);
}
static void start_valid(unsigned number)
{ start(number, s_context.device_id, s_boot_id, 31000); }
static void query(unsigned number)
{
    char json[300]; snprintf(json, sizeof json,
        "{\"protocol_version\":1,\"request_id\":\"55555555-5555-4555-8555-%012u\","
        "\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-%012u\"}}", number, number);
    send_json(json);
}
static bool owner_available(void)
{
    esp_base_storage_claim_t claim = {0};
    const bool available = esp_base_storage_claim(&owner, &claim);
    if (available) assert(esp_base_storage_release(&claim));
    return available;
}
static void run_worker(void) { ota_task(NULL); poll_ota(); }

bool eota_available(void) { return signed_enabled; }
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
    return ESP_OK;
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
    assert(config != NULL && consume != NULL && context != NULL);
    const uint8_t canonical[] = "config-frp-scratch-candidate";
    return consume(canonical, sizeof canonical - 1U, context);
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

esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_request_t *request,
    const esp_base_storage_claim_t *claim)
{
    assert(!strcmp(device_id, s_context.device_id) && request && claim == &s_ota_storage_claim);
    assert(esp_base_storage_claim_active(&s_ota_storage_claim) && task_calls == 0);
    assert(snapshot_calls == 0);
    ++register_calls; registered_request = *request;
    return snapshot_ok ? register_result : ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *receipt)
{
    assert(!strcmp(device_id, s_context.device_id) && receipt);
    assert(esp_base_storage_claim_active(&s_ota_storage_claim));
    ++load_receipt_calls;
    *receipt = (esp_base_ota_receipt_recovery_t){.status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .source_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .image_size_bytes = registered_request.image_size_bytes};
    strcpy(receipt->operation_id, registered_request.operation_id);
    memcpy(receipt->candidate_sha256, registered_request.sha256, 32);
    memcpy(receipt->source_sha256, registered_source_sha256, 32);
    if (receipt_mismatch) receipt->candidate_sha256[0] ^= 1U;
    return load_result;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error)
{
    assert(!strcmp(device_id, s_context.device_id) && !strcmp(operation_id, registered_request.operation_id));
    assert(esp_base_storage_claim_active(&s_ota_storage_claim) && error != EOTA_UPDATE_OK);
    ++failure_record_calls; return failure_record_result;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    assert(!strcmp(device_id, s_context.device_id) && operation_id && view);
    ++query_calls;
    if (worker_active) assert(s_ota_request && !strcmp(s_ota_request->operation_id, operation_id));
    *view = (esp_base_ota_receipt_view_t){.state = worker_active ? ESP_BASE_OTA_OPERATION_RUNNING : query_state,
        .image_size_bytes = 4096, .target_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1};
    strcpy(view->operation_id, operation_id); memset(view->sha256, 0x43, 32);
    return query_result;
}
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack_depth,
                       void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)handle;
    assert(task == ota_task && !strcmp(name, "base_ota") && stack_depth == 12288 && priority == 4 && argument == NULL);
    assert(register_calls == 1 && register_result == ESP_BASE_OTA_RECEIPT_OK && s_ota_active);
    assert((s_ota_request->inbound_stream || s_ota_request == last_decoded_ota_request) && esp_base_storage_claim_active(&s_ota_storage_claim));
    ++task_calls;
    if (worker_created && !defer_ota_worker) task(argument);
    return worker_created ? pdPASS : 0;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void esp_restart(void) { ++restart_calls; }
void vTaskDelay(TickType_t ticks) { fake_now_ms += ticks; }
psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
eota_policy_t esp_base_ota_policy(bool trusted_time)
{ return (eota_policy_t){.trusted_time = trusted_time, .connect_timeout_ms = 5000, .read_timeout_ms = 1000, .idle_timeout_ms = 30000, .total_timeout_ms = 300000}; }
eota_result_t eota_prepare(const eota_policy_t *policy, const eota_image_t *image,
    eota_progress_t progress, void *context, eota_prepared_t *prepared)
{
    assert(policy->trusted_time && image && prepared && progress);
    assert(esp_base_storage_claim_active(&s_ota_storage_claim) && register_calls == 1 && retire_calls == 1);
    ++prepare_calls;
    progress(512, image->image_size_bytes, context);
    *prepared = (eota_prepared_t){.image_size_bytes = image->image_size_bytes};
    memcpy(prepared->sha256, image->sha256, 32);
    return prepare_result;
}
eota_result_t eota_retire_inactive(const eota_policy_t *policy, uint8_t target,
    const uint8_t source_sha256[32])
{
    assert(policy && target == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
        !memcmp(source_sha256, registered_source_sha256, 32));
    assert(esp_base_storage_claim_active(&s_ota_storage_claim));
    ++retire_calls;
    return retire_calls == 1 ? retire_result : second_retire_result;
}
eota_result_t eota_select(const eota_policy_t *policy, const eota_prepared_t *prepared)
{
    assert(policy && prepared && prepare_calls + stream_prepare_calls == 1 && esp_base_storage_claim_active(&s_ota_storage_claim));
    ++select_calls; return select_result;
}
const char *eota_error(eota_result_t error)
{ return error == EOTA_UPDATE_BOOT_STATE_UNKNOWN ? "ota_boot_state_unknown" : "resource_failure"; }
esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation, const eota_prepared_t *prepared,
    esp_base_ota_firmware_set_t *set)
{
    assert(observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED && prepared == NULL && set);
    assert(esp_base_storage_claim_active(&s_ota_storage_claim));
    ++snapshot_calls;
    *set = (esp_base_ota_firmware_set_t){.bootable_count = 2};
    memset(set->running_firmware_sha256, 0x41, 32);
    memset(set->bootable_firmware_sha256[0], 0x41, 32);
    memset(set->bootable_firmware_sha256[1], 0x42, 32);
    return snapshot_ok ? ESP_BASE_OTA_FIRMWARE_OK : ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
}
eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *slots)
{
    assert(policy && slots);
    *slots = (eota_slots_t){.running_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .running_state = EOTA_STATE_VALID};
    return EOTA_UPDATE_OK;
}
eota_result_t eota_sha256_verified_image(const eota_policy_t *policy, uint8_t subtype,
    uint32_t *size, uint8_t digest[32])
{
    assert(policy && subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && size && digest);
    *size = 4096; memset(digest, 0x41, 32); return EOTA_UPDATE_OK;
}
#ifndef ESP_BASE_TEST_REAL_FRP_LISTENER
bool esp_base_frp_management_upload_arm(const char *operation_id, const char *device_id,
    const char *boot_id, uint32_t size, const uint8_t sha256[32], uint64_t now_ms)
{
    assert(register_calls == 1 && task_calls == 0 && esp_base_storage_claim_active(&s_ota_storage_claim));
    assert(!strcmp(operation_id, registered_request.operation_id) && !strcmp(device_id, s_context.device_id) && !strcmp(boot_id, s_boot_id));
    assert(size == 4096 && !memcmp(sha256, registered_request.sha256, 32) && now_ms == fake_now_ms);
    ++upload_arm_calls; return upload_arm_ok;
}
bool esp_base_frp_management_upload_connected(void) { return upload_connected; }
int esp_base_frp_management_upload_read(void *context, uint8_t *bytes, size_t capacity, uint32_t timeout_ms)
{ (void)context; (void)bytes; (void)capacity; (void)timeout_ms; return 0; }
void esp_base_frp_management_upload_cancel(void) {}
bool esp_base_frp_management_upload_finish(int status, const char *body, size_t length, uint32_t timeout_ms)
{
    assert(status == 200 || status == 202 || status == 500);
    assert(timeout_ms == 1000 && (status == 500 ? body == NULL && length == 0 : body && length));
    upload_finish_status = status;
    failure_record_calls_at_upload_finish = failure_record_calls;
    if (status != 500) {
        assert(failure_record_calls == 0 && length < sizeof upload_finish_reply);
        memcpy(upload_finish_reply, body, length); upload_finish_reply[length] = '\0';
        assert(strstr(upload_finish_reply, "\"result\":null"));
        assert(strstr(upload_finish_reply, status == 202 ? "\"state\":\"running\"" : "\"state\":\"unknown\""));
        if (status == 200) assert(strstr(upload_finish_reply, "\"error_code\":\"storage_uncertain\""));
    }
    ++upload_finish_calls; return true;
}
#endif
eota_result_t eota_validate_stream_request(const eota_stream_t *stream)
{ assert(stream && stream->read && stream->image_size_bytes == 4096); ++validate_calls; return validate_result; }
eota_result_t eota_prepare_stream(const eota_policy_t *policy, const eota_stream_t *stream,
    eota_progress_t progress, void *context, eota_prepared_t *prepared)
{
    assert(policy && stream && stream->read == esp_base_frp_management_upload_read && prepared && progress);
    assert(esp_base_storage_claim_active(&s_ota_storage_claim) && upload_arm_calls == 1 && retire_calls == 1);
    ++stream_prepare_calls; progress(512, stream->image_size_bytes, context);
    *prepared = (eota_prepared_t){.image_size_bytes = stream->image_size_bytes};
    memcpy(prepared->sha256, stream->sha256, 32);
    if (upload_cancel_after_prepare) upload_connected = false;
    return prepare_result;
}

static int start_frp(unsigned number)
{
    char json[1500];
    snprintf(json, sizeof json,
        "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-%012u\","
        "\"device_id\":\"22222222-2222-4222-8222-222222222222\","
        "\"target_boot_id\":\"33333333-3333-4333-8333-333333333333\",\"expires_at_uptime_ms\":31000,"
        "\"command\":\"ota.start\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-%012u\","
        "\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
        "\"image_size_bytes\":4096,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}}", number, number);
    size_t length = 0;
    return handle_frp_management(ESP_BASE_FRP_MANAGEMENT_OTA_START, (const uint8_t *)json,
        strlen(json), latest_reply, sizeof latest_reply, &length, NULL);
}
static void check_frp_ota(void)
{
    const esp_base_ota_operation_state_t query_states[] = {
        ESP_BASE_OTA_OPERATION_RUNNING, ESP_BASE_OTA_OPERATION_FAILED, ESP_BASE_OTA_OPERATION_UNKNOWN};
    const int http_states[] = {202, 409, 200};
    for (unsigned i = 0; i < 3; ++i) {
        reset_case(); query_result = ESP_BASE_OTA_RECEIPT_OK; query_state = query_states[i];
        const char *body = "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\",\"request_id\":\"11111111-1111-4111-8111-000000000001\","
            "\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-000000000001\"}}";
        size_t length = 0;
        int http = handle_frp_management(ESP_BASE_FRP_MANAGEMENT_OTA_RESULT, (const uint8_t *)body,
            strlen(body), latest_reply, sizeof latest_reply, &length, NULL);
        if (http != http_states[i]) fprintf(stderr, "query http=%d expected=%d reply=%s\n", http, http_states[i], latest_reply);
        assert(http == http_states[i]);
        assert(query_calls == 1 && register_calls == 0 && owner_available());
    }
    reset_case(); size_t invalid_length = 0;
    assert(handle_frp_management(ESP_BASE_FRP_MANAGEMENT_OTA_START, (const uint8_t *)"{}", 2,
        latest_reply, sizeof latest_reply, &invalid_length, NULL) == 400);
    expect_reply("failed", "invalid_request");

    reset_case(); assert(start_frp(1) == 202); expect_reply("running", NULL);
    assert(s_ota_request && s_ota_request->inbound_stream && s_ota_request->image_url[0] == '\0');
    assert(upload_arm_calls == 1 && register_calls == 1 && !owner_available());
    esp_base_ota_request_t *accepted = s_ota_request;
    start_valid(2); expect_reply("failed", "ota_in_progress");
    assert(s_ota_request == accepted && register_calls == 1);
    assert(start_frp(1) == 202 && upload_arm_calls == 1 && task_calls == 1);
    prepare_result = EOTA_UPDATE_RESOURCE_FAILURE; ota_task(NULL);
    assert(upload_finish_status == 200 && failure_record_calls_at_upload_finish == 0 &&
        failure_record_calls == 0 && !owner_available() && s_ota_active && s_ota_request);
    assert(!strcmp(s_outcomes[s_ota_slot].state, "running"));
    poll_ota();
    assert(stream_prepare_calls == 1 && prepare_calls == 0 && retire_calls == 2 &&
        failure_record_calls == 1 && upload_finish_calls == 1 && owner_available() &&
        !s_ota_request && ota_request_releases == 1);
    assert(!strcmp(s_outcomes[s_ota_slot].state, "failed"));
    reset_case(); assert(start_frp(1) == 202);
    prepare_result = EOTA_UPDATE_RESOURCE_FAILURE;
    failure_record_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    ota_task(NULL);
    assert(upload_finish_status == 200 && failure_record_calls_at_upload_finish == 0 &&
        failure_record_calls == 0 && !owner_available() && s_ota_active && s_ota_request);
    poll_ota();
    assert(failure_record_calls == 1 && !owner_available() && s_config_uncertain &&
        !s_ota_active && !s_ota_request && !strcmp(s_outcomes[s_ota_slot].state, "unknown"));
    reset_case(); start_valid(1);
    assert(start_frp(2) == 409); expect_reply("failed", "ota_in_progress");
    assert(upload_arm_calls == 0 && register_calls == 1);
    prepare_result = EOTA_UPDATE_RESOURCE_FAILURE; run_worker();
    reset_case(); assert(start_frp(1) == 202); run_worker();
    assert(stream_prepare_calls == 1 && prepare_calls == 0 && select_calls == 1 &&
        upload_finish_calls == 1 && restart_calls == 1 && !owner_available());
    reset_case(); upload_cancel_after_prepare = true;
    assert(start_frp(1) == 202); run_worker();
    assert(stream_prepare_calls == 1 && prepare_calls == 0 && select_calls == 0 &&
        retire_calls == 2 && upload_finish_calls == 1 && failure_record_calls == 1 &&
        restart_calls == 0 && owner_available() && !s_ota_request && !s_ota_active);
    assert(!strcmp(s_outcomes[s_ota_slot].state, "failed"));
    reset_case(); upload_arm_ok = false;
    assert(start_frp(1) == 409); expect_reply("failed", "resource_failure");
    assert(task_calls == 0 && failure_record_calls == 1 && owner_available() && !s_ota_request);
    reset_case(); worker_created = false;
    assert(start_frp(1) == 409); expect_reply("failed", "resource_failure");
    assert(upload_finish_calls == 1 && failure_record_calls == 1 && owner_available());
    reset_case(); upload_connected = false;
    assert(start_frp(1) == 202); run_worker();
    assert(fake_now_ms == 6000 && retire_calls == 0 && prepare_calls == 0 &&
        stream_prepare_calls == 0 && upload_finish_calls == 1 && failure_record_calls == 1 && owner_available());
}

static void check_config_flash_observation(void)
{
    reset_case();
    s_candidate = calloc(1U, sizeof *s_candidate);
    assert(s_candidate);
    s_candidate->wifi.configured = true;
    s_candidate->revision = 7U;
    s_trial_active = true; s_trial_deadline = 1234U; s_trial_slot = 0U;
    esp_base_remote_config_t *const original_candidate = s_candidate;
    esp_base_storage_claim_t holder = {0};
    assert(esp_base_storage_claim(&owner, &holder));
    esp_base_flash_observation_t before[ESP_BASE_FLASH_CONSUMER_COUNT], after[ESP_BASE_FLASH_CONSUMER_COUNT];
    esp_base_flash_observation_snapshot(before);
    poll_configuration(fake_now_ms);
    esp_base_flash_observation_snapshot(after);
    assert(s_trial_active && s_candidate == original_candidate && s_trial_deadline == 1234U);
    assert(config_commit_calls == 0U && esp_base_storage_claim_active(&holder));
    assert(after[ESP_BASE_FLASH_CONFIG].acquire_failed_count ==
           before[ESP_BASE_FLASH_CONFIG].acquire_failed_count + 1U);
    assert(after[ESP_BASE_FLASH_CONFIG].completed_claim_count == before[ESP_BASE_FLASH_CONFIG].completed_claim_count);
    assert(fake_now_ms == 1000U); /* Single try neither delays nor extends proof. */
    assert(esp_base_storage_release(&holder));
    poll_configuration(fake_now_ms);
    esp_base_flash_observation_snapshot(after);
    assert(config_commit_calls == 1U && !s_trial_active && s_candidate == NULL && owner_available());
    assert(after[ESP_BASE_FLASH_CONFIG].completed_claim_count == before[ESP_BASE_FLASH_CONFIG].completed_claim_count + 1U);
    assert(after[ESP_BASE_FLASH_CONFIG].counters_valid);
    reset_case();
}

static void check_ota_phase_observation(void)
{
    reset_case();
    FILE *capture = tmpfile();
    assert(capture);
    fflush(stdout);
    const int original_stdout = dup(STDOUT_FILENO);
    assert(original_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    start_valid(1);
    run_worker();
    fflush(stdout);
    assert(dup2(original_stdout, STDOUT_FILENO) >= 0);
    close(original_stdout);
    rewind(capture);
    char output[16384] = {0};
    assert(fread(output, 1U, sizeof output - 1U, capture) > 0U);
    fclose(capture);
    const char *const phases[] = {"retire_inactive", "prepare", "select"};
    const char *cursor = output;
    for (unsigned i = 0; i < sizeof phases / sizeof *phases; ++i) {
        char marker[80];
        snprintf(marker, sizeof marker, "phase=%s boundary=begin", phases[i]);
        cursor = strstr(cursor, marker); assert(cursor);
        snprintf(marker, sizeof marker, "phase=%s boundary=end", phases[i]);
        cursor = strstr(cursor, marker); assert(cursor);
    }
    assert(strstr(cursor, "phase=select boundary=terminal"));
    assert(strstr(cursor, "ESP_BASE_FLASH_IO boot_id="));
    assert(strstr(output, registered_request.operation_id));
    assert(strstr(output, s_guard.entries[s_ota_slot].request_id));
    assert(strstr(output, "snapshot_started_us=") && strstr(output, "snapshot_finished_us="));
    assert(strstr(output, "result=ok stage_uncertain=0"));
    assert(restart_calls == 1U && s_ota_active && s_ota_request);
    reset_case();
}

int main(void)
{
    check_config_flash_observation();
    check_ota_phase_observation();
    check_frp_ota();
    reset_case(); start_valid(1); expect_reply("running", NULL);
    assert(s_ota_request && s_ota_request == last_decoded_ota_request && register_calls == 1 && task_calls == 1);
    assert(!owner_available() && !ota_request_releases && snapshot_calls == 0);
    esp_base_ota_request_t *accepted = s_ota_request;
    query(1); expect_reply("unknown", "ota_operation_not_found");
    start_valid(2); expect_reply("failed", "ota_in_progress");
    assert(s_ota_request == accepted && register_calls == 1 && task_calls == 1 && !ota_request_releases);
    start_valid(1); expect_reply("running", NULL);
    assert(register_calls == 1 && task_calls == 1);
    prepare_result = EOTA_UPDATE_RESOURCE_FAILURE; run_worker(); expect_reply("failed", "resource_failure");
    assert(load_receipt_calls == 1 && retire_calls == 2 && prepare_calls == 1 && select_calls == 0);
    assert(failure_record_calls == 1 && !s_ota_request && !s_ota_active && ota_request_releases == 1 && owner_available());
    poll_ota(); assert(ota_request_releases == 1);

    reset_case(); start_valid(1); run_worker(); expect_reply("running", NULL);
    assert(select_calls == 1 && restart_calls == 1 && failure_record_calls == 0 && !owner_available());
    assert(s_ota_active && s_ota_request && ota_request_releases == 0);

    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        reset_case();
        switch (scenario) {
        case 0: load_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN; break;
        case 1: receipt_mismatch = true; break;
        case 2: retire_result = EOTA_UPDATE_RESOURCE_FAILURE; break;
        case 3: prepare_result = EOTA_UPDATE_RESOURCE_FAILURE; second_retire_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN; break;
        case 4: select_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN; break;
        }
        start_valid(1); run_worker(); expect_reply("unknown", "storage_uncertain");
        assert(!s_ota_active && !s_ota_request && ota_request_releases == 1 && !owner_available());
        assert(s_config_uncertain && s_ota_boot_uncertain && failure_record_calls == 0);
        start_valid(2); expect_reply("failed", "storage_uncertain");
        assert(register_calls == 1 && restart_calls == 0);
    }
    reset_case(); worker_created = false; start_valid(1); expect_reply("failed", "resource_failure");
    assert(failure_record_calls == 1 && !s_ota_request && ota_request_releases == 1 && owner_available());
    reset_case(); worker_created = false; failure_record_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    start_valid(1); expect_reply("unknown", "storage_uncertain");
    assert(!s_ota_request && ota_request_releases == 1 && !owner_available() && s_config_uncertain);
    reset_case(); prepare_result = EOTA_UPDATE_RESOURCE_FAILURE; failure_record_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    start_valid(1); run_worker(); expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 1 && !s_ota_active && !owner_available());

    const esp_base_ota_receipt_result_t refuses[] = {ESP_BASE_OTA_RECEIPT_EXISTS,
        ESP_BASE_OTA_RECEIPT_CONFLICT, ESP_BASE_OTA_RECEIPT_BUSY,
        ESP_BASE_OTA_RECEIPT_SAME_IMAGE, ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE,
        ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN};
    for (unsigned i = 0; i < sizeof refuses / sizeof *refuses; ++i) {
        reset_case(); register_result = refuses[i]; start_valid(1);
        assert(register_calls == 1 && task_calls == 0 && prepare_calls == 0 && !s_ota_request);
        assert(owner_available() == (refuses[i] != ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN));
    }
    reset_case(); snapshot_ok = false; start_valid(1); expect_reply("unknown", "storage_uncertain");
    assert(register_calls == 1 && task_calls == 0 && !owner_available() && s_config_uncertain);
    reset_case(); esp_base_storage_claim_t other_entry = {0};
    assert(esp_base_storage_claim(&owner, &other_entry));
    start_valid(1); expect_reply("failed", "operation_busy");
    assert(register_calls == 0 && esp_base_storage_claim_active(&other_entry));
    assert(esp_base_storage_release(&other_entry));
    reset_case(); esp_base_control_state_set_ota_pending(&s_control_state, true);
    start_valid(1); expect_reply("failed", "ota_verification_pending"); assert(register_calls == 0);
    reset_case(); s_trial_active = true;
    start_valid(1); expect_reply("failed", "configuration_busy"); assert(register_calls == 0);
    reset_case(); network_ready = false;
    start_valid(1); expect_reply("failed", "network_unavailable"); assert(register_calls == 0);
    reset_case(); trusted_time_ready = false;
    start_valid(1); expect_reply("failed", "time_unavailable"); assert(register_calls == 0);
    reset_case(); signed_enabled = false;
    start_valid(1); expect_reply("failed", "ota_signing_unavailable"); assert(register_calls == 0);
    reset_case(); start(1, "99999999-9999-4999-8999-999999999999", s_boot_id, 31000);
    expect_reply("failed", "wrong_device"); assert(register_calls == 0);
    reset_case(); start(1, s_context.device_id, "99999999-9999-4999-8999-999999999999", 31000);
    expect_reply("failed", "wrong_boot"); assert(register_calls == 0);
    reset_case(); start(1, s_context.device_id, s_boot_id, 999);
    expect_reply("expired", "expired"); assert(register_calls == 0);

    reset_case(); query_result = ESP_BASE_OTA_RECEIPT_OK; query(1);
    expect_reply("succeeded", NULL); assert(query_calls == 1 && register_calls == 0);
    assert(strstr(latest_reply, "\"sha256\":\"4343") && strstr(latest_reply, "\"target_slot\":\"ota_1\""));
    reset_case(); send_json("{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-000000000001\",\"command\":\"firmware.status\"}");
    expect_reply("succeeded", NULL);
    assert(strstr(latest_reply, "414141") && owner_available() && !strstr(latest_reply, "package"));
    reset_case(); start_valid(1);
    send_json("{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-000000000002\",\"command\":\"firmware.status\"}");
    expect_reply("unknown", "operation_busy");
    prepare_result = EOTA_UPDATE_RESOURCE_FAILURE; run_worker();
    reset_case(); allow_config_load = true; uint32_t revision = 0;
    assert(esp_base_protocol_load_config(&revision, &owner) == ESP_OK && revision == 7 && config_load_calls == 1);
    assert(owner_available());
    reset_case();
    puts("  protocol_ota_owner passed (real decoder/guard, ownership transfer, prewrite intent, cleanup, unknown gates, firmware-only results)");
    return 0;
}
