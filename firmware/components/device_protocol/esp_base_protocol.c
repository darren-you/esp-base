// SPDX-License-Identifier: Apache-2.0
#include "esp_base_protocol.h"
#include "control_state.h"
#include "esp_base_command.h"
#include "esp_base_identity.h"
#include "esp_base_remote_config.h"
#include "esp_base_mqtt_owner.h"
#include "esp_base_frp_owner.h"
#include "esp_base_frp_management_listener.h"
#include "esp_base_wifi.h"
#include "esp_base_time.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
#include "esp_base_container_product.h"
#include "esp_base_product_ledger_nvs.h"
#include "esp_base_product_package_source.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include "driver/usb_serial_jtag_vfs.h"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/uart_vfs.h"
#else
#error "ESP Base serial control supports only esp32c3 and esp32"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

_Static_assert(ESP_BASE_OTA_PACKAGE_URL_BYTES == ESP_BASE_PRODUCT_PACKAGE_URL_BYTES,
               "OTA package URL must fit the shared HTTPS source contract");

typedef struct {
    const char *device_id;
    const char *firmware_version;
    const char *chip_model;
    uint32_t flash_size_bytes;
    esp_base_remote_config_t config;
    const char *reset_reason;
    esp_base_storage_owner_t *storage_owner;
    esp_base_storage_owner_t *flash_io_owner;
    const efrp_aead_flash_store_t *frp_flash_store;
} protocol_state_t;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
/* C3 has no byte-accessible IRAM heap. Reuse RTC data for the live config;
 * the codec borrows ordinary RAM only during its bounded operation. */
static RTC_DATA_ATTR protocol_state_t s_context;
#else
static protocol_state_t s_context IRAM_BSS_ATTR;
#endif
static char s_boot_id[EBASE_ID_BYTES];
static ebase_request_guard_t s_guard IRAM_BSS_ATTR;
/* A USB/UART command is the only consumer of the full line buffer. Keep it
 * alive only while a physical line is arriving; network owners do not need
 * 9 KiB reserved while no serial input exists. */
static ebase_line_reader_t *s_reader;
static bool s_serial_discard;
static bool s_started, s_config_loaded, s_config_uncertain, s_trial_active;
static bool s_ota_active, s_ota_boot_uncertain;
static esp_base_storage_claim_t s_ota_storage_claim;
static esp_base_storage_claim_t s_product_storage_claim;
typedef enum {
    PRODUCT_WORK_UNCERTAIN = 0,
    PRODUCT_WORK_FAILED,
    PRODUCT_WORK_TRIAL_RUNNING,
    PRODUCT_WORK_RUN_COMPLETE,
    PRODUCT_WORK_RUN_REJECTED,
    PRODUCT_WORK_RUN_BUSY,
} product_work_result_t;
static bool s_product_active, s_product_trial_running;
static size_t s_product_slot;
static ebase_product_package_request_t *s_product_request;
static ebase_product_run_request_t *s_product_run_request;
static char s_product_operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
static uint32_t s_product_operation_sequence;
static uint8_t s_product_fingerprint[32];
static atomic_bool s_product_done;
static atomic_int s_product_result;
static atomic_uint_fast32_t s_product_resolved_sequence;
static uint32_t s_product_trial_sequence;
static uint8_t s_product_trial_event_sha256[32];
static uint8_t s_product_trial_package_sha256[32];
static uint64_t s_product_trial_event_sequence;
static uint64_t s_product_trial_failure_count;
static uint64_t s_product_trial_stable_since_ms;
static uint64_t s_product_trial_last_poll_ms;
/* The control task owns network observations; app_main reads only a bounded
 * snapshot under this nonblocking lock. Never hold it for Flash or TLS. */
static atomic_bool s_firmware_package_verifying;
static atomic_flag s_firmware_health_lock = ATOMIC_FLAG_INIT;
static struct {
    uint8_t package_sha256[32];
    uint64_t event_sequence;
    uint64_t failure_count;
    uint64_t stable_since_ms;
    uint64_t last_poll_ms;
    bool ready;
} s_firmware_health;
static bool s_pending_mqtt_active;
static size_t s_ota_slot;
static esp_base_ota_request_t s_ota_request;
static atomic_bool s_ota_done;
static atomic_bool s_ota_stage_uncertain;
static atomic_int s_ota_result;
static atomic_uint_fast32_t s_ota_received;
static esp_base_control_state_t s_control_state;
static size_t s_trial_slot;
static uint64_t s_trial_deadline;
static esp_base_remote_config_t *s_candidate;
/* Parsed commands and MQTT setup both live within synchronous callbacks.
 * The commit path gets separate temporary storage after the Wi-Fi proof. */
static bool s_reply_mqtt, s_mqtt_revision_set;
static uint32_t s_mqtt_revision;
static bool s_frp_revision_set;
static uint32_t s_frp_revision;
static bool s_frp_restart_pending;
static bool s_mqtt_restart_pending;
static uint64_t s_mqtt_restart_since_ms;
static uint64_t s_frp_restart_since_ms;

const char *esp_base_protocol_boot_id(void)
{
    return s_started && ebase_is_uuid(s_boot_id) ? s_boot_id : NULL;
}
#define MQTT_REPORTED_JSON_BYTES 768u
/* The control task formats one result or one periodic report at a time.
 * MQTT enqueue copies the payload before returning. */
static char s_response_json[1024];
#define FRP_STATUS_REPLAY_SLOTS 8u
typedef struct {
    uint64_t uptime;
    uint32_t revision, free_heap, minimum_free_heap, ota_received, ota_total;
    const char *wifi, *mqtt, *frp, *config, *ota;
} status_snapshot_t;
static struct {
    char request_id[EBASE_ID_BYTES];
    uint64_t expires_at_ms;
    /* Capability names come from static owner state strings, not transient
     * network buffers; retain the first read-only result for same-ID retry. */
    status_snapshot_t status;
} s_frp_status_seen[FRP_STATUS_REPLAY_SLOTS] IRAM_BSS_ATTR;
typedef struct {
    const char *state, *error;
    bool has_status, via_mqtt, has_product_run;
    union {
        status_snapshot_t status;
        struct {
            uint32_t container_sequence;
            ebase_command_kind_t kind;
        } product_run;
    };
} command_outcome_t;
_Static_assert(sizeof(((command_outcome_t *)0)->product_run) <= sizeof(status_snapshot_t),
               "boot-local receipt must reuse existing outcome space");
_Static_assert(sizeof(command_outcome_t) == sizeof(struct {
    const char *state, *error;
    bool has_status, via_mqtt;
    status_snapshot_t status;
}), "boot-local receipts must not grow the 32-slot outcome array");
static command_outcome_t s_outcomes[EBASE_REQUEST_SLOTS] IRAM_BSS_ATTR;

static bool fingerprint_config_bytes(const uint8_t *bytes, size_t length, void *context)
{
    uint8_t *fingerprint = context;
    size_t hash_size = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&hash, (const uint8_t *)"config.set", 10) != PSA_SUCCESS ||
        psa_hash_update(&hash, bytes, length) != PSA_SUCCESS ||
        psa_hash_finish(&hash, fingerprint, 32, &hash_size) != PSA_SUCCESS || hash_size != 32) {
        (void)psa_hash_abort(&hash);
        return false;
    }
    return true;
}

static bool fingerprint_product_uninstall(
    const ebase_product_uninstall_request_t *request, uint8_t fingerprint[32])
{
    static const uint8_t domain[] = "product.uninstall";
    uint8_t bytes[sizeof domain + ESP_BASE_OTA_OPERATION_ID_BYTES + 8U + 32U];
    size_t offset = 0U;
    memcpy(bytes + offset, domain, sizeof domain);
    offset += sizeof domain;
    memcpy(bytes + offset, request->operation_id, ESP_BASE_OTA_OPERATION_ID_BYTES);
    offset += ESP_BASE_OTA_OPERATION_ID_BYTES;
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes[offset++] = (uint8_t)(request->operation_sequence >> shift);
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes[offset++] = (uint8_t)(request->expected_container_sequence >> shift);
    memcpy(bytes + offset, request->package_sha256, 32U);
    offset += 32U;
    size_t written = 0U;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, offset, fingerprint, 32U,
                            &written) == PSA_SUCCESS && written == 32U;
}

static bool fingerprint_product_package(const ebase_command_t *command,
                                        uint8_t fingerprint[32])
{
    const ebase_product_package_request_t *request = command->product_package;
    const char *domain = command->kind == EBASE_PRODUCT_INSTALL_COMMAND ?
                         "product.install" : "product.upgrade";
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    size_t written = 0U;
    uint8_t number[4];
    const uint32_t values[] = {request->operation_sequence,
        request->expected_container_sequence, request->package_size_bytes,
        request->guest_abi_version, request->data_schema_version};
    const uint8_t previous = request->previous_package_present ? 1U : 0U;
    bool valid = psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)domain, strlen(domain) + 1U) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->operation_id,
                        sizeof request->operation_id) == PSA_SUCCESS;
    for (size_t index = 0; valid && index < sizeof values / sizeof values[0]; ++index) {
        for (int shift = 24, byte = 0; shift >= 0; shift -= 8, ++byte)
            number[byte] = (uint8_t)(values[index] >> shift);
        valid = psa_hash_update(&hash, number, sizeof number) == PSA_SUCCESS;
    }
    valid = valid &&
        psa_hash_update(&hash, &previous, sizeof previous) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->previous_package_sha256,
                        sizeof request->previous_package_sha256) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->package_sha256,
                        sizeof request->package_sha256) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->trial_event_sha256,
                        sizeof request->trial_event_sha256) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->package_url,
                        strlen(request->package_url) + 1U) == PSA_SUCCESS &&
        psa_hash_finish(&hash, fingerprint, 32U, &written) == PSA_SUCCESS &&
        written == 32U;
    if (!valid) (void)psa_hash_abort(&hash);
    return valid;
}

static bool fingerprint_product_run(const ebase_command_t *command,
                                    uint8_t fingerprint[32])
{
    const char *domain = command->kind == EBASE_PRODUCT_START_COMMAND ?
        "product.start" : "product.stop";
    uint8_t bytes[sizeof("product.start") + 4U + 32U];
    const size_t domain_size = strlen(domain) + 1U;
    memcpy(bytes, domain, domain_size);
    const uint32_t sequence = command->product_run->expected_container_sequence;
    for (unsigned index = 0; index < 4U; ++index)
        bytes[domain_size + index] = (uint8_t)(sequence >> (24U - 8U * index));
    memcpy(bytes + domain_size + 4U, command->product_run->package_sha256, 32U);
    size_t written = 0U;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, domain_size + 4U + 32U,
        fingerprint, 32U, &written) == PSA_SUCCESS && written == 32U;
}

static bool fingerprint_ota_request(const esp_base_ota_request_t *request,
                                    uint8_t fingerprint[32])
{
    static const uint8_t domain[] = "ota.start";
    const uint32_t numbers[] = {request->image_size_bytes,
        request->package_size_bytes, request->guest_abi_version,
        request->data_schema_version};
    const uint8_t mode = (uint8_t)request->package_mode;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    size_t written = 0U;
    bool valid = psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS &&
        psa_hash_update(&hash, domain, sizeof domain) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->operation_id,
                        sizeof request->operation_id) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->image_url,
                        strlen(request->image_url) + 1U) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->sha256, sizeof request->sha256) == PSA_SUCCESS &&
        psa_hash_update(&hash, &mode, sizeof mode) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->package_sha256,
                        sizeof request->package_sha256) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->trial_event_sha256,
                        sizeof request->trial_event_sha256) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->package_url,
                        strlen(request->package_url) + 1U) == PSA_SUCCESS;
    for (size_t index = 0; valid && index < sizeof numbers / sizeof numbers[0]; ++index) {
        uint8_t number[4];
        for (int shift = 24, byte = 0; shift >= 0; shift -= 8, ++byte)
            number[byte] = (uint8_t)(numbers[index] >> shift);
        valid = psa_hash_update(&hash, number, sizeof number) == PSA_SUCCESS;
    }
    valid = valid && psa_hash_finish(&hash, fingerprint, 32U, &written) == PSA_SUCCESS &&
        written == 32U;
    if (!valid) (void)psa_hash_abort(&hash);
    return valid;
}

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void package_digest_hex(char output[67], const uint8_t digest[32])
{
    static const char digits[] = "0123456789abcdef";
    output[0] = '"';
    for (size_t index = 0; index < 32U; ++index) {
        output[1U + index * 2U] = digits[digest[index] >> 4];
        output[2U + index * 2U] = digits[digest[index] & 15U];
    }
    output[65] = '"';
    output[66] = '\0';
}

static void reported(void)
{
    const uint64_t now = uptime_ms();
    const bool time_ready = esp_base_time_ready();
    const esp_base_frp_snapshot_t frp = esp_base_frp_owner_snapshot();
    ESP_LOGI("base_reported",
        "ESP_BASE_REPORTED schema=1 boot_id=%s uptime_ms=%" PRIu64
        " free_heap=%" PRIu32 " min_free_heap=%" PRIu32
        " device_id=%s firmware=%s chip=%s flash=%" PRIu32 " config_generation=%" PRIu32
        " reset=%s provisioned=%s wifi_state=%s time_ready=%s mqtt_state=%s frp_state=%s frp_attempts=%" PRIu64 " frp_sessions=%" PRIu64 " frp_pongs=%" PRIu64 " frp_active=%" PRIu32 " frp_error=%" PRId32 " ota_received=%" PRIu32 " ota_total=%" PRIu32,
        s_boot_id, now, esp_get_free_heap_size(),
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT), s_context.device_id,
        s_context.firmware_version, s_context.chip_model, s_context.flash_size_bytes,
        s_context.config.revision, s_context.reset_reason,
        s_context.config.wifi.configured ? "true" : "false", esp_base_wifi_state(),
        time_ready ? "true" : "false", esp_base_mqtt_owner_state(), frp.state,
        frp.attempts, frp.ready_sessions, frp.pongs, frp.work_active, frp.error,
        (uint32_t)atomic_load_explicit(&s_ota_received, memory_order_relaxed),
        s_ota_active ? s_ota_request.image_size_bytes : 0);
    esp_base_container_event_observation_t event = {0};
    const esp_base_container_event_observation_result_t observed =
        esp_base_container_product_event_observation(&event);
    const char *event_outcome = observed == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY ? "busy" :
        observed != ESP_BASE_CONTAINER_EVENT_OBSERVED ? "none" :
        !event.runtime_ok ? "runtime_failed" :
        event.guest_result < 0 ? "business_failed" : "succeeded";
    char completed_sequence[24] = "null";
    char guest_result[16] = "null";
    char event_package[67] = "null";
    char completed_event[67] = "null";
    if (observed == ESP_BASE_CONTAINER_EVENT_OBSERVED) {
        (void)snprintf(completed_sequence, sizeof completed_sequence,
                       "%" PRIu64, event.event_sequence);
        package_digest_hex(event_package, event.package_sha256);
        package_digest_hex(completed_event, event.event_sha256);
        if (event.runtime_ok)
            (void)snprintf(guest_result, sizeof guest_result,
                           "%" PRId32, event.guest_result);
    }
    const int size = snprintf(s_response_json, MQTT_REPORTED_JSON_BYTES,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"uptime_ms\":%" PRIu64 ",\"revision\":%" PRIu32 ","
        "\"wifi_state\":\"%s\",\"time_ready\":%s,\"frp_state\":\"%s\","
        "\"last_accepted_event_sequence\":%" PRIu64 ","
        "\"last_completed_event_sequence\":%s,\"last_completed_package_sha256\":%s,"
        "\"last_completed_event_sha256\":%s,"
        "\"last_event_outcome\":\"%s\",\"last_guest_result\":%s}",
        s_context.device_id, s_boot_id, now, s_context.config.revision,
        esp_base_wifi_state(), time_ready ? "true" : "false", frp.state,
        esp_base_mqtt_owner_event_sequence(), completed_sequence, event_package,
        completed_event,
        event_outcome, guest_result);
    if (size > 0 && (size_t)size < MQTT_REPORTED_JSON_BYTES)
        (void)esp_base_mqtt_owner_reported(s_response_json, (size_t)size);
}

static status_snapshot_t snapshot(void)
{
    return (status_snapshot_t){.uptime = uptime_ms(), .revision = s_context.config.revision,
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
        .ota_received = (uint32_t)atomic_load_explicit(&s_ota_received, memory_order_relaxed),
        .ota_total = s_ota_active ? s_ota_request.image_size_bytes : 0,
        .wifi = esp_base_wifi_state(), .mqtt = esp_base_mqtt_owner_state(),
        .frp = esp_base_frp_owner_snapshot().state,
        .config = s_config_uncertain || s_ota_boot_uncertain ? "failed" : "ready",
        .ota = !eota_available() ? "unsupported" : s_ota_active ? "running" : "ready"};
}

static int format_result_json(char *response, size_t capacity,
                              const char *request_id, const char *state,
                              const char *error, const status_snapshot_t *status)
{
    const int written = status ? snprintf(response, capacity,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":%s%s%s,\"state\":\"%s\",\"error_code\":%s%s%s,"
        "\"result\":{\"uptime_ms\":%" PRIu64 ",\"revision\":%" PRIu32
        ",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32
        ",\"ota_received_bytes\":%" PRIu32 ",\"ota_total_bytes\":%" PRIu32
        ",\"capabilities\":{\"wifi\":\"%s\",\"mqtt\":\"%s\","
        "\"frp\":\"%s\",\"config\":\"%s\",\"ota\":\"%s\"}}}",
        s_context.device_id, s_boot_id,
        request_id ? "\"" : "null", request_id ? request_id : "", request_id ? "\"" : "",
        state, error ? "\"" : "null", error ? error : "", error ? "\"" : "",
        status->uptime, status->revision, status->free_heap, status->minimum_free_heap,
        status->ota_received, status->ota_total, status->wifi, status->mqtt,
        status->frp, status->config, status->ota) : snprintf(response, capacity,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":%s%s%s,\"state\":\"%s\",\"error_code\":%s%s%s,\"result\":null}",
        s_context.device_id, s_boot_id,
        request_id ? "\"" : "null", request_id ? request_id : "", request_id ? "\"" : "",
        state, error ? "\"" : "null", error ? error : "", error ? "\"" : "");
    return written >= 0 && (size_t)written < capacity ? written : -1;
}

static int handle_frp_status(const uint8_t *json, size_t json_length,
                             char *response, size_t capacity,
                             size_t *response_length, void *context)
{
    (void)context;
    ebase_request_t request;
    const char *parse_error = ebase_parse_frp_status((const char *)json, json_length, &request);
    const uint64_t now = uptime_ms();
    const char *error = parse_error;
    const char *state = "failed";
    int http_status = 400;
    size_t empty = FRP_STATUS_REPLAY_SLOTS;
    const status_snapshot_t *cached = NULL;
    if (!error && strcmp(request.device_id, s_context.device_id)) {
        error = "wrong_device";
        http_status = 409;
    }
    if (!error) {
        for (size_t i = 0; i < FRP_STATUS_REPLAY_SLOTS; ++i) {
            if (s_frp_status_seen[i].expires_at_ms <= now) {
                if (empty == FRP_STATUS_REPLAY_SLOTS) empty = i;
            } else if (!strcmp(s_frp_status_seen[i].request_id, request.request_id)) {
                cached = &s_frp_status_seen[i].status;
                break;
            }
        }
    }
    if (!error && !cached && empty == FRP_STATUS_REPLAY_SLOTS) error = "capacity_exceeded";
    if (!error && !cached) {
        memcpy(s_frp_status_seen[empty].request_id, request.request_id, EBASE_ID_BYTES);
        s_frp_status_seen[empty].expires_at_ms = now + EBASE_REQUEST_WINDOW_MS;
        s_frp_status_seen[empty].status = snapshot();
        cached = &s_frp_status_seen[empty].status;
    }
    const int formatted = format_result_json(response, capacity,
        parse_error ? NULL : request.request_id,
        error ? state : "succeeded", error, cached && !error ? cached : NULL);
    if (formatted < 0) return 500;
    *response_length = (size_t)formatted;
    return error ? http_status : 200;
}

static void reply(const char *request_id, const char *state, const char *error, const status_snapshot_t *status)
{
    /* USB, MQTT and FRP share one result serializer. Strings are validated
     * UUIDs or closed firmware constants; raw request bytes are never echoed. */
    const int length = format_result_json(s_response_json, sizeof s_response_json,
        request_id && request_id[0] ? request_id : NULL, state, error, status);
    if (length < 0) return;
    if (s_reply_mqtt) {
        (void)esp_base_mqtt_owner_result(s_response_json, (size_t)length);
        return;
    }
    flockfile(stdout);
    fputc('\n', stdout);
    (void)fwrite(s_response_json, 1, (size_t)length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void reply_ota_result(const char *request_id, const esp_base_ota_receipt_view_t *view)
{
    const char *state = view->state == ESP_BASE_OTA_OPERATION_RUNNING ? "running" :
        view->state == ESP_BASE_OTA_OPERATION_SUCCEEDED ? "succeeded" :
        view->state == ESP_BASE_OTA_OPERATION_FAILED ? "failed" : "unknown";
    static const char digits[] = "0123456789abcdef";
    char digest[65];
    for (size_t i = 0; i < 32; ++i) {
        digest[i * 2] = digits[view->sha256[i] >> 4];
        digest[i * 2 + 1] = digits[view->sha256[i] & 15];
    }
    digest[64] = '\0';
    const char *package_mode = view->package_mode == ESP_BASE_OTA_PACKAGE_REUSE ?
        "reuse" : view->package_mode == ESP_BASE_OTA_PACKAGE_WRITE ?
        "write" : "no_package";
    char package_digest[67] = {0};
    const char *package_sha256 = "null";
    if (view->package_mode != ESP_BASE_OTA_NO_PACKAGE) {
        package_digest_hex(package_digest, view->package_sha256);
        package_sha256 = package_digest;
    }
    if (s_reply_mqtt) {
        const int length = snprintf(s_response_json, sizeof s_response_json,
            "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
            "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":%s%s%s,"
            "\"result\":{\"operation_id\":\"%s\",\"sha256\":\"%s\","
            "\"image_size_bytes\":%" PRIu32 ",\"target\":\"%s\",\"target_slot\":\"%s\","
            "\"package_mode\":\"%s\",\"package_sha256\":%s}}",
            s_context.device_id, s_boot_id, request_id, state,
            view->error_code ? "\"" : "null", view->error_code ? view->error_code : "",
            view->error_code ? "\"" : "", view->operation_id, digest, view->image_size_bytes,
            ESP_BASE_OTA_TARGET,
            view->target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1",
            package_mode, package_sha256);
        if (length > 0 && (size_t)length < sizeof s_response_json)
            (void)esp_base_mqtt_owner_result(s_response_json, (size_t)length);
        return;
    }
    flockfile(stdout);
    printf("\n{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
           "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":",
           s_context.device_id, s_boot_id, request_id, state);
    if (view->error_code) printf("\"%s\"", view->error_code); else printf("null");
    printf(",\"result\":{\"operation_id\":\"%s\",\"sha256\":\"%s\","
           "\"image_size_bytes\":%" PRIu32 ",\"target\":\"%s\",\"target_slot\":\"%s\","
           "\"package_mode\":\"%s\",\"package_sha256\":%s}}\n",
           view->operation_id, digest, view->image_size_bytes, ESP_BASE_OTA_TARGET,
           view->target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1",
           package_mode, package_sha256);
    fflush(stdout);
    funlockfile(stdout);
}

static bool find_product_run(const char *operation_id, size_t *out_slot)
{
    for (size_t slot = 0; slot < s_guard.count; ++slot) {
        if (s_outcomes[slot].has_product_run &&
            !strcmp(s_guard.entries[slot].request_id, operation_id)) {
            if (out_slot != NULL) *out_slot = slot;
            return true;
        }
    }
    return false;
}

static void reply_product_run(const char *request_id, size_t slot)
{
    const command_outcome_t *outcome = &s_outcomes[slot];
    const char *kind = outcome->product_run.kind == EBASE_PRODUCT_START_COMMAND ?
        "start" : "stop";
    const int length = snprintf(s_response_json, sizeof s_response_json,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":%s%s%s,"
        "\"result\":{\"operation_id\":\"%s\",\"operation_sequence\":null,"
        "\"kind\":\"%s\",\"container_sequence\":%" PRIu32 "}}",
        s_context.device_id, s_boot_id, request_id, outcome->state,
        outcome->error ? "\"" : "null", outcome->error ? outcome->error : "",
        outcome->error ? "\"" : "", s_guard.entries[slot].request_id, kind,
        outcome->product_run.container_sequence);
    if (length <= 0 || (size_t)length >= sizeof s_response_json) return;
    if (s_reply_mqtt) {
        (void)esp_base_mqtt_owner_result(s_response_json, (size_t)length);
        return;
    }
    flockfile(stdout);
    fputc('\n', stdout);
    (void)fwrite(s_response_json, 1, (size_t)length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void reply_product_result(const char *request_id,
                                 const ebase_product_record_t *record)
{
    static const char digits[] = "0123456789abcdef";
    char digest[65];
    for (size_t i = 0; i < 32; ++i) {
        digest[2 * i] = digits[record->package_sha256[i] >> 4];
        digest[2 * i + 1] = digits[record->package_sha256[i] & 15U];
    }
    digest[64] = '\0';
    const char *kind = record->kind == EBASE_PRODUCT_INSTALL ? "install" :
        record->kind == EBASE_PRODUCT_UPGRADE ? "upgrade" : "uninstall";
    const char *state = record->state == EBASE_PRODUCT_SUCCEEDED ? "succeeded" :
        record->state == EBASE_PRODUCT_FAILED ? "failed" : "unknown";
    const char *error = record->state == EBASE_PRODUCT_FAILED ? "product_operation_failed" :
        record->state == EBASE_PRODUCT_PREPARED ? "product_operation_unresolved" : NULL;
    const int length = snprintf(s_response_json, sizeof s_response_json,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":%s%s%s,"
        "\"result\":{\"operation_id\":\"%s\",\"operation_sequence\":%" PRIu32
        ",\"kind\":\"%s\",\"package_sha256\":\"%s\","
        "\"container_sequence\":%" PRIu32 ",\"result_code\":%u}}",
        s_context.device_id, s_boot_id, request_id, state,
        error ? "\"" : "null", error ? error : "", error ? "\"" : "",
        record->operation_id, record->sequence, kind, digest,
        record->container_sequence, (unsigned)record->result_code);
    if (length < 0 || (size_t)length >= sizeof s_response_json) return;
    if (s_reply_mqtt) {
        (void)esp_base_mqtt_owner_result(s_response_json, (size_t)length);
        return;
    }
    flockfile(stdout);
    fputc('\n', stdout);
    (void)fwrite(s_response_json, 1, (size_t)length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void *protocol_work_alloc(size_t size);

static void reply_product_status(const char *request_id,
                                 const ebase_product_ledger_t *ledger,
                                 const esp_base_container_binding_snapshot_t *binding,
                                 const esp_base_container_active_product_t *active)
{
    const char *pending = NULL;
    if (ledger->count && ledger->records[ledger->count - 1U].state == EBASE_PRODUCT_PREPARED)
        pending = ledger->records[ledger->count - 1U].operation_id;
    char next[16];
    if (ledger->high_watermark == UINT32_MAX) {
        memcpy(next, "null", 5);
    } else {
        (void)snprintf(next, sizeof next, "%" PRIu32, ledger->high_watermark + 1U);
    }
    char firmware_digest[67];
    package_digest_hex(firmware_digest, binding->firmware_sha256);
    char package_abi[16] = "null", package_schema[16] = "null";
    if (binding->package_present) {
        (void)snprintf(package_abi, sizeof package_abi, "%" PRIu32, binding->package_guest_abi_version);
        (void)snprintf(package_schema, sizeof package_schema, "%" PRIu32, binding->package_data_schema_version);
    }
    char digest[67];
    const char *package_sha256 = "null";
    if (binding->package_present) {
        package_digest_hex(digest, binding->package_sha256);
        package_sha256 = digest;
    }
    /* Absent views own zero lengths and nullable string pointers. Carry the
     * authorized lengths from the snapshot, without a nullable strlen call. */
    const size_t capacity = 1024U + active->product_id_size_bytes +
        active->product_version_size_bytes;
    char *response = protocol_work_alloc(capacity);
    if (response == NULL) {
        reply(request_id, "unknown", "resource_failure", NULL);
        return;
    }
    int length = snprintf(response, capacity,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":\"%s\",\"state\":\"succeeded\",\"error_code\":null,"
        "\"result\":{\"operation_sequence_high_watermark\":%" PRIu32
        ",\"next_operation_sequence\":%s,\"pending_operation_id\":%s%s%s,"
        "\"container_sequence\":%" PRIu32 ",\"package_sha256\":%s,"
        "\"firmware_sha256\":%s,\"runtime_guest_abi_version\":%" PRIu32 ","
        "\"package_guest_abi_version\":%s,\"package_data_schema_version\":%s,\"active_product\":",
        s_context.device_id, s_boot_id, request_id, ledger->high_watermark, next,
        pending ? "\"" : "null", pending ? pending : "", pending ? "\"" : "",
        binding->container_sequence, package_sha256, firmware_digest,
        binding->runtime_guest_abi_version, package_abi, package_schema);
    if (length >= 0 && (size_t)length < capacity) {
        char active_digest[67];
        package_digest_hex(active_digest, active->package_sha256);
        const int tail = active->present ? snprintf(response + length, capacity - (size_t)length,
            "{\"product_id\":\"%s\",\"product_version\":\"%s\",\"package_sha256\":%s,"
            "\"guest_abi_version\":%" PRIu32 ",\"data_schema_version\":%" PRIu32 ","
            "\"is_trial\":%s,\"operation_id\":%s%s%s}}}",
            active->product_id, active->product_version, active_digest,
            active->guest_abi_version, active->data_schema_version, active->is_trial ? "true" : "false",
            active->is_trial ? "\"" : "null", active->is_trial ? active->operation_id : "",
            active->is_trial ? "\"" : "") :
            snprintf(response + length, capacity - (size_t)length, "null}}");
        if (tail < 0 || (size_t)tail >= capacity - (size_t)length) length = -1;
        else length += tail;
    } else length = -1;
    if (length < 0) reply(request_id, "unknown", "resource_failure", NULL);
    else if (s_reply_mqtt) (void)esp_base_mqtt_owner_result(response, (size_t)length);
    else {
        flockfile(stdout);
        fputc('\n', stdout);
        (void)fwrite(response, 1, (size_t)length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        funlockfile(stdout);
    }
    free(response);
}

static void emit_outcome(size_t slot, bool via_mqtt)
{
    command_outcome_t *out = &s_outcomes[slot];
    const bool previous_route = s_reply_mqtt;
    s_reply_mqtt = via_mqtt;
    reply(s_guard.entries[slot].request_id, out->state, out->error, out->has_status ? &out->status : NULL);
    s_reply_mqtt = previous_route;
}

static void store_outcome(size_t slot, const char *state, const char *error, bool status)
{
    const command_outcome_t previous = s_outcomes[slot];
    s_outcomes[slot] = (command_outcome_t){.state = state, .error = error,
        .has_status = status, .via_mqtt = previous.via_mqtt,
        .has_product_run = !status && previous.has_product_run};
    if (s_outcomes[slot].has_product_run)
        s_outcomes[slot].product_run = previous.product_run;
    if (status) s_outcomes[slot].status = snapshot();
}

static void save_outcome(size_t slot, const char *state, const char *error, bool status)
{
    store_outcome(slot, state, error, status);
    emit_outcome(slot, s_outcomes[slot].via_mqtt);
}

static const char *admission_error(ebase_admission_t decision)
{
    static const char *const errors[] = {NULL, NULL, "invalid_identity", "wrong_device",
        "wrong_boot", "expired", "invalid_deadline", "request_conflict", "capacity_exceeded"};
    return errors[decision];
}

static const char *restart_write_error(void)
{
    if (s_ota_active) return "ota_in_progress";
    if (s_product_active || s_frp_restart_pending || s_mqtt_restart_pending) return "operation_busy";
    if (s_ota_boot_uncertain) return "ota_boot_state_unknown";
    if (s_trial_active) return "configuration_busy";
    if (s_config_uncertain) return "storage_uncertain";
    return NULL;
}

static int handle_frp_restart(const uint8_t *json, size_t json_length,
                              char *response, size_t capacity,
                              size_t *response_length)
{
    ebase_request_t request;
    const char *parse_error = ebase_parse_frp_restart((const char *)json, json_length, &request);
    const char *error = parse_error;
    const char *state = "failed";
    int http_status = 400;
    bool schedule = false;
    const uint64_t now = uptime_ms();
    size_t slot = 0;
    if (!error) {
        const ebase_admission_t decision = ebase_admit(&s_guard, &request,
            s_context.device_id, s_boot_id, now, &slot);
        http_status = 409;
        if (decision == EBASE_REPLAY) {
            /* The global table also contains USB/MQTT admissions. Reading its
             * result never executes the write again or changes its route. */
            state = s_outcomes[slot].state;
            error = s_outcomes[slot].error;
        } else if (decision != EBASE_ACCEPT) {
            error = admission_error(decision);
            if (decision == EBASE_EXPIRED) state = "expired";
        } else {
            error = esp_base_control_state_ota_pending(&s_control_state) ?
                "ota_verification_pending" : restart_write_error();
            state = error ? "failed" : "running";
            s_outcomes[slot].via_mqtt = false;
            store_outcome(slot, state, error, false);
            schedule = !error;
        }
        if (!error && !strcmp(state, "running")) http_status = 202;
    }
    const int formatted = format_result_json(response, capacity,
        parse_error ? NULL : request.request_id, state, error, NULL);
    if (formatted < 0) {
        if (schedule) store_outcome(slot, "failed", "resource_failure", false);
        return 500;
    }
    *response_length = (size_t)formatted;
    if (schedule) {
        s_frp_restart_since_ms = now;
        s_frp_restart_pending = true;
    }
    return http_status;
}

static int handle_frp_management(esp_base_frp_management_command_t command,
                                 const uint8_t *json, size_t json_length,
                                 char *response, size_t capacity,
                                 size_t *response_length, void *context)
{
    if (command == ESP_BASE_FRP_MANAGEMENT_STATUS)
        return handle_frp_status(json, json_length, response, capacity, response_length, context);
    if (command == ESP_BASE_FRP_MANAGEMENT_RESTART)
        return handle_frp_restart(json, json_length, response, capacity, response_length);
    return 500;
}

static void poll_frp_restart(uint64_t now)
{
    if (!s_frp_restart_pending || now - s_frp_restart_since_ms < 100U) return;
    /* Allow the authenticated receipt to drain, with a hard two-second bound
     * even if the peer stops reading. Receipt loss remains unconfirmed. */
    if (esp_base_frp_management_listener_response_pending() &&
        now - s_frp_restart_since_ms < 2000U) return;
    s_frp_restart_pending = false;
    esp_restart();
}

static void poll_mqtt_restart(uint64_t now)
{
    if (!s_mqtt_restart_pending || now - s_mqtt_restart_since_ms < 100U) return;
    /* Return from the MESSAGE handler so the owner can observe the exact
     * receipt PUBACK. A lost receipt remains unconfirmed; restart is bounded. */
    if (!esp_base_mqtt_owner_restart_result_acknowledged() &&
        now - s_mqtt_restart_since_ms < 2000U) return;
    s_mqtt_restart_pending = false;
    esp_restart();
}

static void restore_committed(uint64_t now)
{
    if (esp_base_wifi_apply(&s_context.config.wifi, now) != ESP_OK) s_config_uncertain = true;
}

static void *protocol_work_alloc(size_t size)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    /* Classic ESP32 uses single-core 8BIT IRAM for transient protocol data.
     * Task stacks remain in ordinary byte-addressable internal RAM. */
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT);
#else
    return malloc(size);
#endif
}

bool esp_base_protocol_recover_product_package(
    const esp_base_storage_claim_t *claim)
{
    if (!esp_base_storage_claim_active(claim) || s_context.flash_io_owner == NULL)
        return false;
    ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
    if (ledger == NULL) return false;
    const ebase_product_ledger_io_t io =
        ebase_product_ledger_nvs_io(s_context.flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
    if (opened == EBASE_LEDGER_UNINITIALIZED) {
        free(ledger);
        return true;
    }
    if (opened != EBASE_LEDGER_OK) {
        free(ledger);
        return false;
    }
    if (ledger->count == 0U ||
        ledger->records[ledger->count - 1U].state != EBASE_PRODUCT_PREPARED ||
        ledger->records[ledger->count - 1U].kind == EBASE_PRODUCT_UNINSTALL) {
        free(ledger);
        return true;
    }
    const ebase_product_record_t pending = ledger->records[ledger->count - 1U];
    if (pending.kind != EBASE_PRODUCT_INSTALL &&
        pending.kind != EBASE_PRODUCT_UPGRADE) {
        free(ledger);
        return false;
    }
    uint32_t resolved_sequence = 0U;
    const esp_base_container_package_recovery_t recovered =
        esp_base_container_product_recover_pending_package(
            claim, s_boot_id, pending.operation_id, pending.container_sequence,
            pending.package_sha256, &resolved_sequence);
    if (recovered == ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN ||
        resolved_sequence == 0U) {
        free(ledger);
        return false;
    }
    const ebase_product_ledger_result_t finished = ebase_product_ledger_finish(
        ledger, &io, pending.sequence, pending.operation_id, pending.fingerprint,
        recovered == ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED ?
            EBASE_PRODUCT_SUCCEEDED : EBASE_PRODUCT_FAILED,
        recovered == ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED ? 0U : 1U,
        resolved_sequence);
    free(ledger);
    return finished == EBASE_LEDGER_OK;
}

bool esp_base_protocol_prepare_product_ledger(
    const esp_base_storage_claim_t *claim, bool product_empty)
{
    if (!esp_base_storage_claim_active(claim) || s_context.flash_io_owner == NULL)
        return false;
    ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
    if (ledger == NULL) return false;
    const ebase_product_ledger_io_t io =
        ebase_product_ledger_nvs_io(s_context.flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
    if (opened == EBASE_LEDGER_OK) {
        if (ledger->count &&
            ledger->records[ledger->count - 1U].state == EBASE_PRODUCT_PREPARED) {
            const ebase_product_record_t pending = ledger->records[ledger->count - 1U];
            if (pending.kind != EBASE_PRODUCT_UNINSTALL ||
                pending.container_sequence == UINT32_MAX) {
                free(ledger);
                return false;
            }
            const esp_base_container_uninstall_recovery_t recovered =
                esp_base_container_product_reconcile_uninstall(
                    claim, pending.operation_id, pending.container_sequence,
                    pending.package_sha256);
            if ((recovered == ESP_BASE_CONTAINER_UNINSTALL_RECOVERED && !product_empty) ||
                (recovered == ESP_BASE_CONTAINER_UNINSTALL_NOT_COMMITTED && product_empty) ||
                recovered == ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN) {
                free(ledger);
                return false;
            }
            const ebase_product_ledger_result_t finished = ebase_product_ledger_finish(
                ledger, &io, pending.sequence, pending.operation_id,
                pending.fingerprint,
                recovered == ESP_BASE_CONTAINER_UNINSTALL_RECOVERED ?
                    EBASE_PRODUCT_SUCCEEDED : EBASE_PRODUCT_FAILED,
                recovered == ESP_BASE_CONTAINER_UNINSTALL_RECOVERED ? 0U : 1U,
                pending.container_sequence +
                    (recovered == ESP_BASE_CONTAINER_UNINSTALL_RECOVERED ? 1U : 0U));
            free(ledger);
            return finished == EBASE_LEDGER_OK;
        }
        free(ledger);
        return true;
    }
    if (!product_empty || opened != EBASE_LEDGER_UNINITIALIZED ||
        !esp_base_container_product_pristine_baseline(claim)) {
        free(ledger);
        return false;
    }
    const ebase_product_ledger_result_t initialized =
        ebase_product_ledger_initialize_empty(ledger, &io);
    free(ledger);
    return initialized == EBASE_LEDGER_OK;
}

static void clear_candidate(void)
{
    if (s_candidate == NULL) return;
    memset(s_candidate, 0, sizeof *s_candidate);
    free(s_candidate);
    s_candidate = NULL;
}

static bool claim_config_flash_io(esp_base_storage_owner_t *owner,
                                  esp_base_storage_claim_t *claim)
{
    return owner != NULL && claim != NULL && esp_base_storage_claim(owner, claim);
}

static esp_err_t load_config_with_flash_io(
    esp_base_remote_config_t *config, esp_base_storage_owner_t *owner)
{
    esp_base_storage_claim_t claim = {0};
    if (!claim_config_flash_io(owner, &claim)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = esp_base_remote_config_load(config);
    return esp_base_storage_release(&claim) ? result : ESP_FAIL;
}

static void poll_configuration(uint64_t now)
{
    if (!s_trial_active) return;
    const bool ready = s_candidate->wifi.configured ? esp_base_wifi_ready() :
        !strcmp(esp_base_wifi_state(), "unconfigured");
    if (ready && now < s_trial_deadline) {
        /* Another short Flash operation may finish on a later control pass.
         * Keep both the candidate and its original proof deadline intact. */
        esp_base_storage_claim_t claim = {0};
        if (!claim_config_flash_io(s_context.flash_io_owner, &claim)) return;
        /* The control task is the sole reader/writer of s_context.config after
         * startup. The FRP owner borrows this boot-long canonical storage;
         * native network workers own copies made when they are created. */
        esp_base_remote_config_t *work = protocol_work_alloc(sizeof *work);
        if (work == NULL) {
            const bool released = esp_base_storage_release(&claim);
            s_trial_active = false;
            clear_candidate();
            if (released) {
                restore_committed(now);
                save_outcome(s_trial_slot, "failed", "resource_failure", false);
            } else {
                s_config_uncertain = true;
                ebase_wifi_config_t disabled = {0};
                (void)esp_base_wifi_apply(&disabled, now);
                save_outcome(s_trial_slot, "unknown", "storage_uncertain", false);
            }
            return;
        }
        esp_err_t error = esp_base_remote_config_commit_verified(
            s_candidate, s_candidate->revision, &s_context.config, work);
        if (!esp_base_storage_release(&claim)) error = ESP_BASE_CONFIG_UNCERTAIN;
        s_trial_active = false;
        clear_candidate();
        bool reloaded = false;
        if (error == ESP_BASE_CONFIG_UNCERTAIN) {
            /* A write error may follow a durable commit. Decode may also
             * modify its output before returning an error. Reload into the
             * existing work and publish only a complete admitted config. */
            reloaded = load_config_with_flash_io(
                work, s_context.flash_io_owner) == ESP_OK;
            if (reloaded) s_context.config = *work;
        }
        memset(work, 0, sizeof *work);
        free(work);
        if (error == ESP_OK) {
            save_outcome(s_trial_slot, "succeeded", NULL, true);
        } else if (error == ESP_BASE_CONFIG_UNCERTAIN) {
            s_config_uncertain = true;
            if (reloaded) {
                restore_committed(now);
            } else {
                ebase_wifi_config_t disabled = {0};
                (void)esp_base_wifi_apply(&disabled, now);
            }
            save_outcome(s_trial_slot, "unknown", "storage_uncertain", false);
        } else {
            restore_committed(now);
            save_outcome(s_trial_slot, "failed", "storage_failure", false);
        }
    } else if (now >= s_trial_deadline || !strcmp(esp_base_wifi_state(), "failed")) {
        s_trial_active = false;
        clear_candidate();
        restore_committed(now);
        save_outcome(s_trial_slot, "failed", "connection_proof_failed", false);
    }
}

static void ota_progress(uint32_t received, uint32_t total, void *context)
{
    (void)total;
    (void)context;
    atomic_store_explicit(&s_ota_received, received, memory_order_relaxed);
}

static void ota_task(void *argument)
{
    (void)argument;
    if (!esp_base_container_product_ota_ready(s_ota_request.package_mode)) {
        atomic_store_explicit(&s_ota_result, EOTA_UPDATE_RESOURCE_FAILURE,
                              memory_order_relaxed);
        atomic_store_explicit(&s_ota_done, true, memory_order_release);
        vTaskDelete(NULL);
        return;
    }
    const eota_policy_t policy = esp_base_ota_policy(true);
    eota_image_t image = {
        .image_url = s_ota_request.image_url,
        .image_size_bytes = s_ota_request.image_size_bytes,
    };
    memcpy(image.sha256, s_ota_request.sha256, sizeof image.sha256);
    esp_base_ota_receipt_recovery_t receipt = {0};
    eota_result_t result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (esp_base_ota_receipt_load_for_recovery(
            s_context.device_id, &receipt) != ESP_BASE_OTA_RECEIPT_OK ||
        receipt.status != ESP_BASE_OTA_RECEIPT_PREPARED ||
        receipt.package_mode != s_ota_request.package_mode ||
        memcmp(receipt.package_sha256, s_ota_request.package_sha256, 32) != 0 ||
        memcmp(receipt.trial_event_sha256, s_ota_request.trial_event_sha256, 32) != 0 ||
        receipt.package_size_bytes != s_ota_request.package_size_bytes ||
        receipt.guest_abi_version != s_ota_request.guest_abi_version ||
        receipt.data_schema_version != s_ota_request.data_schema_version ||
        strcmp(receipt.operation_id, s_ota_request.operation_id) != 0 ||
        receipt.image_size_bytes != image.image_size_bytes ||
        memcmp(receipt.candidate_sha256, image.sha256, sizeof image.sha256) != 0) {
        /* A missing or changed durable intent cannot authorize app erasure. */
        atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
    } else {
        if (receipt.package_mode != ESP_BASE_OTA_NO_PACKAGE) {
            esp_base_ota_receipt_snapshot_t source = {0};
            if (!esp_base_container_product_snapshot_for_ota(
                    &s_ota_storage_claim, &s_ota_request, &source) ||
                source.container_enabled != receipt.container_enabled ||
                source.container_sequence != receipt.container_sequence ||
                memcmp(source.source_sha256, receipt.source_sha256, 32) != 0 ||
                memcmp(source.inactive_sha256, receipt.inactive_sha256, 32) != 0 ||
                source.source_package_present != receipt.source_package_present ||
                memcmp(source.source_package_sha256, receipt.source_package_sha256, 32) != 0 ||
                source.source_package_size_bytes != receipt.source_package_size_bytes ||
                source.source_guest_abi_version != receipt.source_guest_abi_version ||
                source.source_data_schema_version != receipt.source_data_schema_version) {
                atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
                goto done;
            }
        }
        /* The original receipt, physical A/B and ECS2 sequence are all known
         * before the first target write. Retire B physically, then retire its
         * persistent binding, and only then let IDF download C into the slot. */
        result = eota_retire_inactive(&policy, receipt.target_subtype,
                                      receipt.source_sha256);
        if (result == EOTA_UPDATE_OK &&
            esp_base_container_product_retire_inactive(
                &s_ota_storage_claim, &receipt) != ESP_BASE_CONTAINER_RETIRE_COMPLETE) {
            result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
        if (result != EOTA_UPDATE_OK) {
            atomic_store_explicit(&s_ota_stage_uncertain, true,
                                  memory_order_relaxed);
        }
    }
    if (result == EOTA_UPDATE_OK) {
        eota_prepared_t prepared = {0};
        result = eota_prepare(&policy, &image, ota_progress, NULL, &prepared);
        if (result != EOTA_UPDATE_OK) {
            /* The target can contain a partially written C with a bootable
             * header. Keep the owner until the same receipt is reconciled. */
            atomic_store_explicit(&s_ota_stage_uncertain, true,
                                  memory_order_relaxed);
        }
        if (result != EOTA_UPDATE_OK) goto done;
        if (receipt.source_package_present &&
            !esp_base_container_product_stop_confirmed(&s_ota_storage_claim)) {
            /* Source resources must be reclaimed before package preparation.
             * C already exists: a failed stop keeps the original claim. */
            atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
            result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
            goto done;
        }
        esp_base_container_stage_result_t stage =
            esp_base_container_product_stage_firmware(
                &s_ota_storage_claim, &prepared, &receipt);
        if (receipt.package_mode == ESP_BASE_OTA_PACKAGE_WRITE &&
            stage == ESP_BASE_CONTAINER_STAGE_WRITING) {
            esp_base_product_package_source_t *source = esp_base_time_ready() ?
                esp_base_product_package_source_open(s_ota_request.package_url,
                    receipt.package_size_bytes, true) : NULL;
            if (source != NULL) {
                stage = esp_base_container_product_write_staged_firmware_package(
                    &s_ota_storage_claim, &prepared, &receipt,
                    esp_base_product_package_source_read, source);
                const bool complete = esp_base_product_package_source_complete(source);
                esp_base_product_package_source_close(source);
                if (!complete) stage = ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
            } else stage = ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
        }
        if ((receipt.container_enabled &&
             stage == ESP_BASE_CONTAINER_STAGE_PREPARED) ||
            (!receipt.container_enabled &&
             stage == ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED)) {
            result = eota_select(&policy, &prepared);
            if (result != EOTA_UPDATE_OK) {
                atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
            }
        } else {
            atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
            result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
    }
done:
    atomic_store_explicit(&s_ota_result, result, memory_order_relaxed);
    atomic_store_explicit(&s_ota_done, true, memory_order_release);
    vTaskDelete(NULL);
}

static void poll_ota(void)
{
    if (!s_ota_active || !atomic_load_explicit(&s_ota_done, memory_order_acquire)) return;
    const eota_result_t result =
        (eota_result_t)atomic_load_explicit(&s_ota_result, memory_order_relaxed);
    if (atomic_load_explicit(&s_ota_stage_uncertain, memory_order_relaxed)) {
        s_config_uncertain = true;
        s_ota_boot_uncertain = true;
        esp_base_control_state_set_ota_download_active(&s_control_state, false);
        s_ota_active = false;
        atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
        save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
        memset(&s_ota_request, 0, sizeof s_ota_request);
        return;
    }
    if (result == EOTA_UPDATE_OK) {
        /* The slot is selected but not yet confirmed. A new boot must pass the
         * local self-test and stability window before it becomes valid. */
        save_outcome(s_ota_slot, "running", NULL, false);
        (void)fsync(STDOUT_FILENO);
        esp_restart();
        return;
    }
    if (result == EOTA_UPDATE_BOOT_STATE_UNKNOWN) {
        s_ota_boot_uncertain = true;
        ESP_LOGE("base_ota", "ESP_BASE_OTA_RECOVERY_REQUIRED selector readback unavailable; avoid resetting device");
    } else {
        const esp_base_ota_receipt_result_t saved = esp_base_ota_receipt_record_failure(
            s_context.device_id, s_ota_request.operation_id, result);
        if (saved != ESP_BASE_OTA_RECEIPT_OK) {
            s_config_uncertain = true;
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
            save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
            memset(&s_ota_request, 0, sizeof s_ota_request);
            return;
        }
        if (!esp_base_storage_release(&s_ota_storage_claim)) {
            s_ota_boot_uncertain = true;
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            atomic_store_explicit(&s_ota_done, false, memory_order_relaxed);
            save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
            memset(&s_ota_request, 0, sizeof s_ota_request);
            return;
        }
    }
    esp_base_control_state_set_ota_download_active(&s_control_state, false);
    s_ota_active = false;
    atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
    save_outcome(s_ota_slot, result == EOTA_UPDATE_BOOT_STATE_UNKNOWN ? "unknown" : "failed",
                 eota_error(result), false);
    memset(&s_ota_request, 0, sizeof s_ota_request);
}

static bool product_binding_unchanged(
    const ebase_product_package_request_t *request,
    const esp_base_container_binding_snapshot_t *binding,
    uint32_t expected_sequence)
{
    return binding->container_sequence == expected_sequence &&
        binding->package_present == request->previous_package_present &&
        (!binding->package_present ||
         memcmp(binding->package_sha256, request->previous_package_sha256, 32) == 0);
}

static void product_task(void *argument)
{
    (void)argument;
    const ebase_product_package_request_t *request = s_product_request;
    product_work_result_t outcome = PRODUCT_WORK_UNCERTAIN;
    uint32_t resolved_sequence = 0U;
    if (s_product_run_request != NULL &&
        esp_base_storage_claim_active(&s_product_storage_claim)) {
        const esp_base_container_run_result_t ran =
            esp_base_container_product_set_running(&s_product_storage_claim,
                s_product_run_request->running,
                s_boot_id, s_product_run_request->expected_container_sequence,
                s_product_run_request->package_sha256);
        outcome = ran == ESP_BASE_CONTAINER_RUN_COMPLETE ? PRODUCT_WORK_RUN_COMPLETE :
            ran == ESP_BASE_CONTAINER_RUN_REJECTED ? PRODUCT_WORK_RUN_REJECTED :
            ran == ESP_BASE_CONTAINER_RUN_BUSY ? PRODUCT_WORK_RUN_BUSY : PRODUCT_WORK_UNCERTAIN;
        goto done;
    }
    if (!esp_base_storage_claim_active(&s_product_storage_claim) ||
        request == NULL) goto done;
    ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
    if (ledger == NULL) goto done;
    const ebase_product_ledger_io_t io =
        ebase_product_ledger_nvs_io(s_context.flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
    const ebase_product_record_t *pending = ledger->count ?
        &ledger->records[ledger->count - 1U] : NULL;
    const bool intent_valid = opened == EBASE_LEDGER_OK && pending != NULL &&
        pending->state == EBASE_PRODUCT_PREPARED &&
        pending->kind == (request->previous_package_present ?
            EBASE_PRODUCT_UPGRADE : EBASE_PRODUCT_INSTALL) &&
        pending->sequence == request->operation_sequence &&
        pending->container_sequence == request->expected_container_sequence &&
        strcmp(pending->operation_id, request->operation_id) == 0 &&
        memcmp(pending->fingerprint, s_product_fingerprint, 32) == 0 &&
        memcmp(pending->package_sha256, request->package_sha256, 32) == 0;
    free(ledger);
    if (!intent_valid) goto done;
    if (!esp_base_time_ready()) {
        outcome = PRODUCT_WORK_FAILED;
        resolved_sequence = request->expected_container_sequence;
        goto done;
    }
    esp_base_product_package_source_t *source =
        esp_base_product_package_source_open(request->package_url,
            request->package_size_bytes, true);
    if (source == NULL) {
        outcome = PRODUCT_WORK_FAILED;
        resolved_sequence = request->expected_container_sequence;
        goto done;
    }
    esp_base_container_package_request_t candidate = {
        .expected_sequence = request->expected_container_sequence,
        .previous_package_present = request->previous_package_present,
        .package_size_bytes = request->package_size_bytes,
        .guest_abi_version = request->guest_abi_version,
        .data_schema_version = request->data_schema_version,
    };
    memcpy(candidate.operation_id, request->operation_id,
           sizeof candidate.operation_id);
    memcpy(candidate.previous_package_sha256, request->previous_package_sha256,
           sizeof candidate.previous_package_sha256);
    memcpy(candidate.package_sha256, request->package_sha256,
           sizeof candidate.package_sha256);
    /* A rejected write returns the read-back ABORTED sequence here; zero
     * means reservation never happened. */
    uint32_t prepared_sequence = 0U;
    const esp_base_container_prepare_result_t prepared =
        esp_base_container_product_prepare_package(&s_product_storage_claim,
            &candidate, esp_base_product_package_source_read, source,
            &prepared_sequence);
    const bool transfer_complete = esp_base_product_package_source_complete(source);
    esp_base_product_package_source_close(source);
    if (prepared != ESP_BASE_CONTAINER_PREPARED) {
        /* BUSY is returned before Container reserves a candidate. As for a
         * deterministic rejection, fail only after proving the old binding
         * and sequence are still intact. */
        if (prepared == ESP_BASE_CONTAINER_PREPARE_REJECTED ||
            prepared == ESP_BASE_CONTAINER_PREPARE_BUSY) {
            esp_base_container_binding_snapshot_t binding = {0};
            if (esp_base_container_product_binding_snapshot(
                    &s_product_storage_claim, &binding) ==
                    ESP_BASE_CONTAINER_BINDING_OK &&
                product_binding_unchanged(request, &binding,
                    prepared_sequence ? prepared_sequence :
                    request->expected_container_sequence)) {
                outcome = PRODUCT_WORK_FAILED;
                resolved_sequence = binding.container_sequence;
            }
        }
        goto done;
    }
    if (!transfer_complete) {
        if (esp_base_container_product_abandon_prepared_package(
                &s_product_storage_claim, prepared_sequence,
                request->operation_id, request->package_sha256,
                &resolved_sequence))
            outcome = PRODUCT_WORK_FAILED;
        goto done;
    }
    if (request->previous_package_present &&
        !esp_base_container_product_stop_confirmed(&s_product_storage_claim))
        goto done;
    const esp_base_container_boot_result_t trial =
        esp_base_container_product_start_package_trial(
            &s_product_storage_claim, prepared_sequence,
            request->operation_id, s_boot_id, request->trial_event_sha256);
    if (trial == ESP_BASE_CONTAINER_RUNNING) {
        resolved_sequence = prepared_sequence + 1U;
        outcome = PRODUCT_WORK_TRIAL_RUNNING;
        goto done;
    }
    if (esp_base_container_product_abandon_package_trial(
            &s_product_storage_claim, prepared_sequence + 1U,
            request->operation_id)) {
        resolved_sequence = prepared_sequence + 2U;
    } else if (!esp_base_container_product_abandon_prepared_package(
            &s_product_storage_claim, prepared_sequence,
            request->operation_id, request->package_sha256,
            &resolved_sequence)) {
        goto done;
    }
    if (esp_base_container_product_boot(&s_product_storage_claim, s_boot_id) ==
        (request->previous_package_present ?
            ESP_BASE_CONTAINER_RUNNING : ESP_BASE_CONTAINER_EMPTY))
        outcome = PRODUCT_WORK_FAILED;
done:
    atomic_store_explicit(&s_product_resolved_sequence, resolved_sequence,
                          memory_order_relaxed);
    atomic_store_explicit(&s_product_result, outcome, memory_order_relaxed);
    atomic_store_explicit(&s_product_done, true, memory_order_release);
    vTaskDelete(NULL);
}

static void poll_product(void)
{
    if (!s_product_active ||
        !atomic_load_explicit(&s_product_done, memory_order_acquire)) return;
    atomic_store_explicit(&s_product_done, false, memory_order_relaxed);
    const product_work_result_t outcome =
        (product_work_result_t)atomic_load_explicit(&s_product_result,
                                                    memory_order_relaxed);
    const uint32_t resolved_sequence = (uint32_t)atomic_load_explicit(
        &s_product_resolved_sequence, memory_order_relaxed);
    if (s_product_run_request != NULL) {
        memset(s_product_run_request, 0, sizeof *s_product_run_request);
        free(s_product_run_request);
        s_product_run_request = NULL;
        if ((outcome == PRODUCT_WORK_RUN_COMPLETE ||
             outcome == PRODUCT_WORK_RUN_REJECTED || outcome == PRODUCT_WORK_RUN_BUSY) &&
            esp_base_storage_release(&s_product_storage_claim)) {
            s_product_active = false;
            save_outcome(s_product_slot,
                outcome == PRODUCT_WORK_RUN_COMPLETE ? "succeeded" : "failed",
                outcome == PRODUCT_WORK_RUN_COMPLETE ? NULL :
                outcome == PRODUCT_WORK_RUN_BUSY ? "operation_busy" : "product_precondition_conflict",
                false);
        } else {
            s_config_uncertain = true;
            save_outcome(s_product_slot, "unknown", "product_state_uncertain", false);
        }
        return;
    }
    if (outcome == PRODUCT_WORK_TRIAL_RUNNING && s_product_request != NULL) {
        memcpy(s_product_trial_event_sha256, s_product_request->trial_event_sha256,
               sizeof s_product_trial_event_sha256);
        memcpy(s_product_trial_package_sha256, s_product_request->package_sha256,
               sizeof s_product_trial_package_sha256);
    }
    free(s_product_request);
    s_product_request = NULL;
    if (outcome == PRODUCT_WORK_FAILED && resolved_sequence != 0U) {
        ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
        if (ledger != NULL) {
            const ebase_product_ledger_io_t io =
                ebase_product_ledger_nvs_io(s_context.flash_io_owner);
            const ebase_product_ledger_result_t opened =
                ebase_product_ledger_open(ledger, &io);
            const ebase_product_ledger_result_t finished =
                opened == EBASE_LEDGER_OK ? ebase_product_ledger_finish(
                    ledger, &io, s_product_operation_sequence,
                    s_product_operation_id, s_product_fingerprint,
                    EBASE_PRODUCT_FAILED, 1U, resolved_sequence) : opened;
            free(ledger);
            if (finished == EBASE_LEDGER_OK &&
                esp_base_storage_release(&s_product_storage_claim)) {
                s_product_active = false;
                save_outcome(s_product_slot, "failed", "product_operation_failed", false);
                return;
            }
        }
    } else if (outcome == PRODUCT_WORK_TRIAL_RUNNING &&
               resolved_sequence != 0U &&
               esp_base_storage_release(&s_product_storage_claim)) {
        s_product_trial_running = true;
        s_product_trial_sequence = resolved_sequence;
        s_product_trial_event_sequence = 0U;
        s_product_trial_failure_count = 0U;
        s_product_trial_stable_since_ms = 0U;
        s_product_trial_last_poll_ms = 0U;
        save_outcome(s_product_slot, "running", NULL, false);
        return;
    }
    /* The ledger or candidate state is not provable. Retain the claim until
     * next-boot reconciliation; no other write may reinterpret this result. */
    s_config_uncertain = true;
    save_outcome(s_product_slot, "unknown", "storage_uncertain", false);
}

#define PRODUCT_TRIAL_STABLE_MS 30000U
#define PRODUCT_TRIAL_PROGRESS_GAP_MS 1000U

static bool observe_business_trial_window(uint64_t now,
    const uint8_t package_sha256[32], uint64_t *event_sequence,
    uint64_t *failure_count, uint64_t *stable_since_ms, uint64_t *last_poll_ms)
{
    const bool online = esp_base_wifi_ready() && esp_base_time_ready() &&
        esp_base_mqtt_owner_ready();
    esp_base_container_trial_event_snapshot_t snapshot = {0};
    const bool observed = esp_base_container_product_trial_event_snapshot(&snapshot);
    const bool representative = observed &&
        snapshot.representative_event_sequence != 0U &&
        memcmp(snapshot.package_sha256, package_sha256, 32) == 0;
    const bool failure_changed = observed &&
        snapshot.failure_count != *failure_count;
    if (!online || !esp_base_container_product_event_accepting() ||
        !representative || failure_changed ||
        (observed && snapshot.failure_count == UINT64_MAX) ||
        (*last_poll_ms != 0U &&
         (now < *last_poll_ms ||
          now - *last_poll_ms > PRODUCT_TRIAL_PROGRESS_GAP_MS))) {
        *event_sequence = 0U;
        *stable_since_ms = 0U;
        *last_poll_ms = now;
        if (observed) *failure_count = snapshot.failure_count;
        return false;
    }
    if (*event_sequence != snapshot.representative_event_sequence ||
        *stable_since_ms == 0U) {
        *event_sequence = snapshot.representative_event_sequence;
        *stable_since_ms = now;
    }
    *last_poll_ms = now;
    if (now < *stable_since_ms ||
        now - *stable_since_ms < PRODUCT_TRIAL_STABLE_MS) return false;
    return esp_base_container_product_trial_quiescent();
}

static void poll_firmware_package_health(uint64_t now)
{
    if (!atomic_load_explicit(&s_firmware_package_verifying, memory_order_acquire) ||
        atomic_flag_test_and_set_explicit(&s_firmware_health_lock, memory_order_acquire))
        return;
    s_firmware_health.ready = observe_business_trial_window(now,
        s_firmware_health.package_sha256, &s_firmware_health.event_sequence,
        &s_firmware_health.failure_count, &s_firmware_health.stable_since_ms,
        &s_firmware_health.last_poll_ms);
    atomic_flag_clear_explicit(&s_firmware_health_lock, memory_order_release);
}

static void poll_product_trial_health(uint64_t now)
{
    if (!s_product_active || !s_product_trial_running || s_config_uncertain ||
        !observe_business_trial_window(now, s_product_trial_package_sha256,
            &s_product_trial_event_sequence, &s_product_trial_failure_count,
            &s_product_trial_stable_since_ms, &s_product_trial_last_poll_ms)) return;

    esp_base_storage_claim_t claim = {0};
    if (!esp_base_storage_claim(s_context.storage_owner, &claim)) return;
    uint32_t confirmed_sequence = 0U;
    const esp_base_container_trial_confirm_result_t trial_commit =
        esp_base_container_product_confirm_package_trial(
        &claim, s_product_trial_sequence, s_product_operation_id,
        s_product_trial_event_sequence, s_product_trial_event_sha256,
        s_product_trial_failure_count,
        &confirmed_sequence);
    if (trial_commit == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED &&
        esp_base_storage_release(&claim)) return;
    bool confirmed = trial_commit == ESP_BASE_CONTAINER_CONFIRM_CONFIRMED;
    if (confirmed && confirmed_sequence != s_product_trial_sequence + 2U) confirmed = false;
    if (confirmed) {
        ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
        if (ledger != NULL) {
            const ebase_product_ledger_io_t io =
                ebase_product_ledger_nvs_io(s_context.flash_io_owner);
            confirmed = ebase_product_ledger_open(ledger, &io) == EBASE_LEDGER_OK &&
                ebase_product_ledger_finish(ledger, &io,
                    s_product_operation_sequence, s_product_operation_id,
                    s_product_fingerprint, EBASE_PRODUCT_SUCCEEDED, 0U,
                    confirmed_sequence) == EBASE_LEDGER_OK;
            free(ledger);
        } else confirmed = false;
    }
    if (confirmed && esp_base_storage_release(&claim)) {
        s_product_trial_running = false;
        s_product_active = false;
        save_outcome(s_product_slot, "succeeded", NULL, false);
        return;
    }
    /* A false commit may already have persisted HEALTH_VERIFIED. Keep the
     * storage claim until next-boot reconciliation proves its final state. */
    s_product_storage_claim = claim;
    s_product_trial_running = false;
    s_config_uncertain = true;
    save_outcome(s_product_slot, "unknown", "storage_uncertain", false);
}

static void poll_product_trial_failure(void)
{
    if (!s_product_active || !s_product_trial_running ||
        esp_base_container_product_event_accepting()) return;
    esp_base_storage_claim_t claim = {0};
    if (!esp_base_storage_claim(s_context.storage_owner, &claim)) return;
    bool recovered = esp_base_container_product_abandon_package_trial(
        &claim, s_product_trial_sequence, s_product_operation_id);
    esp_base_container_binding_snapshot_t binding = {0};
    if (recovered) {
        recovered = esp_base_container_product_binding_snapshot(&claim, &binding) ==
            ESP_BASE_CONTAINER_BINDING_OK &&
            binding.container_sequence == s_product_trial_sequence + 1U;
    }
    if (recovered) {
        const esp_base_container_boot_result_t boot =
            esp_base_container_product_boot(&claim, s_boot_id);
        recovered = boot == (binding.package_present ?
            ESP_BASE_CONTAINER_RUNNING : ESP_BASE_CONTAINER_EMPTY);
    }
    if (recovered) {
        ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
        if (ledger != NULL) {
            const ebase_product_ledger_io_t io =
                ebase_product_ledger_nvs_io(s_context.flash_io_owner);
            recovered = ebase_product_ledger_open(ledger, &io) == EBASE_LEDGER_OK &&
                ebase_product_ledger_finish(ledger, &io,
                    s_product_operation_sequence, s_product_operation_id,
                    s_product_fingerprint, EBASE_PRODUCT_FAILED, 1U,
                    binding.container_sequence) == EBASE_LEDGER_OK;
            free(ledger);
        } else recovered = false;
    }
    if (recovered && esp_base_storage_release(&claim)) {
        s_product_trial_running = false;
        s_product_active = false;
        save_outcome(s_product_slot, "failed", "product_runtime_failed", false);
        return;
    }
    /* A partial rollback cannot be retried as a new product operation. */
    s_product_storage_claim = claim;
    s_product_trial_running = false;
    s_config_uncertain = true;
    save_outcome(s_product_slot, "unknown", "storage_uncertain", false);
}

static void finish_product_without_worker(size_t slot, ebase_product_ledger_t *ledger,
                                          bool uncertain, const char *state,
                                          const char *error)
{
    if (uncertain) {
        s_config_uncertain = true;
        state = "unknown";
        error = "storage_uncertain";
    } else if (!esp_base_storage_release(&s_product_storage_claim)) {
        s_config_uncertain = true;
        state = "unknown";
        error = "storage_uncertain";
    }
    free(ledger);
    save_outcome(slot, state, error, false);
}

static void handle_product_run(size_t slot, ebase_command_t *command)
{
    /* Read-only collision check: a boot-local ID cannot hide a durable receipt. */
    ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
    if (ledger == NULL) {
        save_outcome(slot, "failed", "resource_failure", false); return;
    }
    const ebase_product_ledger_io_t io =
        ebase_product_ledger_nvs_io(s_context.flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
    ebase_product_record_t prior = {0};
    const ebase_product_ledger_result_t found = opened == EBASE_LEDGER_OK ?
        ebase_product_ledger_query(ledger, command->request.request_id, &prior) : opened;
    const bool pending = opened == EBASE_LEDGER_OK && ledger->count != 0U &&
        ledger->records[ledger->count - 1U].state == EBASE_PRODUCT_PREPARED;
    free(ledger);
    if (found != EBASE_LEDGER_UNKNOWN) {
        save_outcome(slot, found == EBASE_LEDGER_OK ? "failed" : "unknown",
            found == EBASE_LEDGER_OK ? "product_operation_conflict" :
            found == EBASE_LEDGER_BUSY ? "operation_busy" :
            found == EBASE_LEDGER_UNINITIALIZED ? "product_ledger_uninitialized" :
            "storage_uncertain", false);
        return;
    }
    s_outcomes[slot].has_product_run = true;
    s_outcomes[slot].product_run.kind = command->kind;
    s_outcomes[slot].product_run.container_sequence =
        command->product_run->expected_container_sequence;
    if (esp_base_control_state_ota_pending(&s_control_state)) {
        save_outcome(slot, "failed", "ota_verification_pending", false); return;
    }
    if (s_ota_active || s_trial_active || s_product_active) {
        save_outcome(slot, "failed", "operation_busy", false); return;
    }
    if (s_config_uncertain || s_ota_boot_uncertain) {
        save_outcome(slot, "unknown", "storage_uncertain", false); return;
    }
    if (pending) {
        save_outcome(slot, "failed", "product_previous_unresolved", false); return;
    }
    if (!esp_base_container_product_configured()) {
        save_outcome(slot, "failed", "product_not_configured", false); return;
    }
    s_product_storage_claim = (esp_base_storage_claim_t){0};
    if (!esp_base_storage_claim(s_context.storage_owner, &s_product_storage_claim)) {
        save_outcome(slot, "failed", "operation_busy", false); return;
    }
    esp_base_container_binding_snapshot_t binding = {0};
    const esp_base_container_binding_result_t bound =
        esp_base_container_product_binding_snapshot(&s_product_storage_claim, &binding);
    if (bound != ESP_BASE_CONTAINER_BINDING_OK) {
        finish_product_without_worker(slot, NULL,
            bound == ESP_BASE_CONTAINER_BINDING_UNCERTAIN, "failed",
            bound == ESP_BASE_CONTAINER_BINDING_BUSY ? "operation_busy" :
            bound == ESP_BASE_CONTAINER_BINDING_RESOURCE_FAILURE ? "resource_failure" :
            "product_not_configured");
        return;
    }
    if (!binding.package_present ||
        binding.container_sequence != command->product_run->expected_container_sequence ||
        memcmp(binding.package_sha256, command->product_run->package_sha256, 32) != 0) {
        finish_product_without_worker(slot, NULL, false,
            "failed", "product_precondition_conflict"); return;
    }
    s_product_run_request = command->product_run;
    command->payload = NULL;
    command->payload_size_bytes = 0U;
    s_product_slot = slot;
    s_product_active = true;
    s_product_trial_running = false;
    atomic_store_explicit(&s_product_done, false, memory_order_relaxed);
    if (xTaskCreate(product_task, "base_product", 12288, NULL, 4, NULL) != pdPASS) {
        memset(s_product_run_request, 0, sizeof *s_product_run_request);
        free(s_product_run_request);
        s_product_run_request = NULL;
        s_product_active = false;
        finish_product_without_worker(slot, NULL, false,
            "failed", "resource_failure"); return;
    }
    save_outcome(slot, "running", NULL, false);
}

static void handle_product_package(size_t slot, const ebase_command_t *command)
{
    const ebase_product_package_request_t *request = command->product_package;
    if (find_product_run(request->operation_id, NULL)) {
        save_outcome(slot, "failed", "product_operation_conflict", false); return;
    }
    const ebase_product_kind_t kind = command->kind == EBASE_PRODUCT_INSTALL_COMMAND ?
        EBASE_PRODUCT_INSTALL : EBASE_PRODUCT_UPGRADE;
    if (s_product_active) {
        if (s_outcomes[s_product_slot].has_product_run) {
            save_outcome(slot, "failed", "operation_busy", false); return;
        }
        if (!strcmp(s_product_operation_id, request->operation_id)) {
            if (memcmp(s_product_fingerprint, command->request.fingerprint, 32) == 0)
                save_outcome(slot, s_config_uncertain ? "unknown" : "running",
                             s_config_uncertain ? "storage_uncertain" : NULL, false);
            else save_outcome(slot, "failed", "product_operation_conflict", false);
        } else save_outcome(slot, "failed", "operation_busy", false);
        return;
    }
    if (esp_base_control_state_ota_pending(&s_control_state)) {
        save_outcome(slot, "failed", "ota_verification_pending", false); return;
    }
    if (s_ota_active || s_trial_active || s_product_active) {
        save_outcome(slot, "failed", "operation_busy", false); return;
    }
    if (s_config_uncertain || s_ota_boot_uncertain) {
        save_outcome(slot, "unknown", "storage_uncertain", false); return;
    }
    if (!esp_base_container_product_configured()) {
        save_outcome(slot, "failed", "product_not_configured", false); return;
    }
    s_product_storage_claim = (esp_base_storage_claim_t){0};
    if (!esp_base_storage_claim(s_context.storage_owner,
                                &s_product_storage_claim)) {
        save_outcome(slot, "failed", "operation_busy", false); return;
    }
    ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
    if (ledger == NULL) {
        finish_product_without_worker(slot, NULL, false,
                                      "failed", "resource_failure");
        return;
    }
    const ebase_product_ledger_io_t io =
        ebase_product_ledger_nvs_io(s_context.flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
    if (opened != EBASE_LEDGER_OK) {
        finish_product_without_worker(slot, ledger,
            opened == EBASE_LEDGER_UNCERTAIN,
            opened == EBASE_LEDGER_UNINITIALIZED || opened == EBASE_LEDGER_BUSY ?
                "unknown" : "failed",
            opened == EBASE_LEDGER_UNINITIALIZED ? "product_ledger_uninitialized" :
            opened == EBASE_LEDGER_BUSY ? "operation_busy" : "storage_uncertain");
        return;
    }
    ebase_product_record_t prior = {0};
    const ebase_product_ledger_result_t found = ebase_product_ledger_query(
        ledger, request->operation_id, &prior);
    if (found == EBASE_LEDGER_OK) {
        finish_product_without_worker(slot, ledger, false,
            prior.kind != kind ||
            memcmp(prior.fingerprint, command->request.fingerprint, 32) != 0 ?
                "failed" : prior.state == EBASE_PRODUCT_SUCCEEDED ?
                "succeeded" : prior.state == EBASE_PRODUCT_FAILED ?
                "failed" : "unknown",
            prior.kind != kind ||
            memcmp(prior.fingerprint, command->request.fingerprint, 32) != 0 ?
                "product_operation_conflict" :
                prior.state == EBASE_PRODUCT_PREPARED ?
                "product_operation_unresolved" :
                prior.state == EBASE_PRODUCT_FAILED ?
                "product_operation_failed" : NULL);
        return;
    }
    if (found != EBASE_LEDGER_UNKNOWN) {
        finish_product_without_worker(slot, ledger, true,
                                      "unknown", "storage_uncertain");
        return;
    }
    /* Existing IDs remain read-only when network or trusted time disappears. */
    if (!esp_base_wifi_ready() || !esp_base_time_ready()) {
        finish_product_without_worker(slot, ledger, false, "failed",
            !esp_base_wifi_ready() ? "network_unavailable" : "time_unavailable");
        return;
    }
    if (request->expected_container_sequence > UINT32_MAX - 5U) {
        finish_product_without_worker(slot, ledger, false,
            "failed", "product_sequence_conflict");
        return;
    }
    esp_base_container_binding_snapshot_t binding = {0};
    const esp_base_container_binding_result_t bound =
        esp_base_container_product_binding_snapshot(&s_product_storage_claim,
                                                    &binding);
    if (bound != ESP_BASE_CONTAINER_BINDING_OK) {
        finish_product_without_worker(slot, ledger,
            bound == ESP_BASE_CONTAINER_BINDING_UNCERTAIN,
            bound == ESP_BASE_CONTAINER_BINDING_BUSY ? "failed" : "unknown",
            bound == ESP_BASE_CONTAINER_BINDING_BUSY ? "operation_busy" :
            "storage_uncertain");
        return;
    }
    if (binding.container_sequence != request->expected_container_sequence ||
        binding.package_present != request->previous_package_present ||
        (binding.package_present &&
         memcmp(binding.package_sha256,
                request->previous_package_sha256, 32) != 0)) {
        finish_product_without_worker(slot, ledger, false,
            "failed", "product_precondition_conflict");
        return;
    }
    ebase_product_package_request_t *work = protocol_work_alloc(sizeof *work);
    if (work == NULL) {
        finish_product_without_worker(slot, ledger, false,
                                      "failed", "resource_failure");
        return;
    }
    ebase_product_record_t intent = {
        .sequence = request->operation_sequence,
        .container_sequence = request->expected_container_sequence,
        .kind = kind,
        .state = EBASE_PRODUCT_PREPARED,
    };
    memcpy(intent.operation_id, request->operation_id, sizeof intent.operation_id);
    memcpy(intent.fingerprint, command->request.fingerprint, 32);
    memcpy(intent.package_sha256, request->package_sha256, 32);
    const ebase_product_ledger_result_t begun = ebase_product_ledger_begin(
        ledger, &io, &intent);
    if (begun != EBASE_LEDGER_OK) {
        free(work);
        finish_product_without_worker(slot, ledger,
            begun == EBASE_LEDGER_UNCERTAIN, "failed",
            begun == EBASE_LEDGER_PENDING ? "product_previous_unresolved" :
            begun == EBASE_LEDGER_STALE_SEQUENCE ||
            begun == EBASE_LEDGER_SEQUENCE_GAP ||
            begun == EBASE_LEDGER_EXHAUSTED ? "product_sequence_conflict" :
            begun == EBASE_LEDGER_BUSY ? "operation_busy" :
            "product_operation_conflict");
        return;
    }
    *work = *request;
    s_product_request = work;
    s_product_operation_sequence = request->operation_sequence;
    memcpy(s_product_operation_id, request->operation_id,
           sizeof s_product_operation_id);
    memcpy(s_product_fingerprint, command->request.fingerprint, 32);
    s_product_slot = slot;
    s_product_active = true;
    s_product_trial_running = false;
    atomic_store_explicit(&s_product_done, false, memory_order_relaxed);
    if (xTaskCreate(product_task, "base_product", 12288, NULL, 4, NULL) != pdPASS) {
        s_product_active = false;
        s_product_request = NULL;
        free(work);
        const ebase_product_ledger_result_t finished = ebase_product_ledger_finish(
            ledger, &io, intent.sequence, intent.operation_id,
            intent.fingerprint, EBASE_PRODUCT_FAILED, 1U,
            intent.container_sequence);
        finish_product_without_worker(slot, ledger,
            finished != EBASE_LEDGER_OK,
            "failed", "resource_failure");
        return;
    }
    free(ledger);
    save_outcome(slot, "running", NULL, false);
}

static void handle_product_uninstall(size_t slot, const ebase_command_t *command)
{
    const ebase_product_uninstall_request_t *request = command->product_uninstall;
    if (find_product_run(request->operation_id, NULL)) {
        save_outcome(slot, "failed", "product_operation_conflict", false); return;
    }
    if (esp_base_control_state_ota_pending(&s_control_state)) {
        save_outcome(slot, "failed", "ota_verification_pending", false); return;
    }
    if (s_ota_active || s_trial_active || s_product_active) {
        save_outcome(slot, "failed", "operation_busy", false); return;
    }
    if (s_config_uncertain || s_ota_boot_uncertain) {
        save_outcome(slot, "failed", "storage_uncertain", false); return;
    }
    if (!esp_base_container_product_configured()) {
        save_outcome(slot, "failed", "product_not_configured", false); return;
    }
    s_product_storage_claim = (esp_base_storage_claim_t){0};
    if (!esp_base_storage_claim(s_context.storage_owner, &s_product_storage_claim)) {
        save_outcome(slot, "failed", "operation_busy", false); return;
    }
    ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
    if (ledger == NULL) {
        finish_product_without_worker(slot, NULL, false, "failed", "resource_failure");
        return;
    }
    const ebase_product_ledger_io_t io =
        ebase_product_ledger_nvs_io(s_context.flash_io_owner);
    const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
    if (opened != EBASE_LEDGER_OK) {
        finish_product_without_worker(slot, ledger, false,
            opened == EBASE_LEDGER_UNINITIALIZED || opened == EBASE_LEDGER_UNCERTAIN ?
                "unknown" : "failed",
            opened == EBASE_LEDGER_UNINITIALIZED ? "product_ledger_uninitialized" :
            opened == EBASE_LEDGER_BUSY ? "operation_busy" : "storage_uncertain");
        return;
    }

    ebase_product_record_t prior = {0};
    const ebase_product_ledger_result_t found = ebase_product_ledger_query(
        ledger, request->operation_id, &prior);
    if (found == EBASE_LEDGER_OK) {
        if (prior.kind != EBASE_PRODUCT_UNINSTALL ||
            memcmp(prior.fingerprint, command->request.fingerprint, 32) != 0 ||
            memcmp(prior.package_sha256, request->package_sha256, 32) != 0) {
            finish_product_without_worker(slot, ledger, false, "failed",
                                     "product_operation_conflict");
        } else {
            finish_product_without_worker(slot, ledger, false,
                prior.state == EBASE_PRODUCT_SUCCEEDED ? "succeeded" :
                prior.state == EBASE_PRODUCT_FAILED ? "failed" : "unknown",
                prior.state == EBASE_PRODUCT_FAILED ? "product_operation_failed" :
                prior.state == EBASE_PRODUCT_PREPARED ? "product_operation_unresolved" : NULL);
        }
        return;
    }
    if (found != EBASE_LEDGER_UNKNOWN) {
        finish_product_without_worker(slot, ledger, false, "unknown", "storage_uncertain");
        return;
    }
    esp_base_container_binding_snapshot_t binding = {0};
    const esp_base_container_binding_result_t bound =
        esp_base_container_product_binding_snapshot(&s_product_storage_claim, &binding);
    if (bound != ESP_BASE_CONTAINER_BINDING_OK) {
        finish_product_without_worker(slot, ledger,
            bound == ESP_BASE_CONTAINER_BINDING_UNCERTAIN,
            bound == ESP_BASE_CONTAINER_BINDING_BUSY ? "failed" : "unknown",
            bound == ESP_BASE_CONTAINER_BINDING_BUSY ? "operation_busy" :
            bound == ESP_BASE_CONTAINER_BINDING_NOT_CONFIGURED ? "product_not_configured" :
            "storage_uncertain");
        return;
    }
    if (!binding.package_present) {
        finish_product_without_worker(slot, ledger, false, "failed", "product_not_installed");
        return;
    }
    if (binding.container_sequence != request->expected_container_sequence ||
        memcmp(binding.package_sha256, request->package_sha256, 32) != 0) {
        finish_product_without_worker(slot, ledger, false, "failed",
                                 "product_precondition_conflict");
        return;
    }
    ebase_product_record_t intent = {
        .sequence = request->operation_sequence,
        .container_sequence = request->expected_container_sequence,
        .kind = EBASE_PRODUCT_UNINSTALL,
        .state = EBASE_PRODUCT_PREPARED,
    };
    memcpy(intent.operation_id, request->operation_id, sizeof intent.operation_id);
    memcpy(intent.fingerprint, command->request.fingerprint, 32);
    memcpy(intent.package_sha256, request->package_sha256, 32);
    const ebase_product_ledger_result_t begun = ebase_product_ledger_begin(
        ledger, &io, &intent);
    if (begun != EBASE_LEDGER_OK) {
        finish_product_without_worker(slot, ledger, begun == EBASE_LEDGER_UNCERTAIN,
            begun == EBASE_LEDGER_BUSY ? "unknown" : "failed",
            begun == EBASE_LEDGER_PENDING ? "product_previous_unresolved" :
            begun == EBASE_LEDGER_STALE_SEQUENCE || begun == EBASE_LEDGER_SEQUENCE_GAP ||
            begun == EBASE_LEDGER_EXHAUSTED ? "product_sequence_conflict" :
            begun == EBASE_LEDGER_BUSY ? "operation_busy" : "product_operation_conflict");
        return;
    }
    const esp_base_container_uninstall_result_t uninstalled =
        esp_base_container_product_uninstall(&s_product_storage_claim,
            request->operation_id, request->expected_container_sequence,
            request->package_sha256);
    if (uninstalled == ESP_BASE_CONTAINER_UNINSTALL_COMPLETE) {
        if (esp_base_container_product_boot(&s_product_storage_claim, s_boot_id) !=
            ESP_BASE_CONTAINER_EMPTY) {
            finish_product_without_worker(slot, ledger, true, "unknown", "storage_uncertain");
            return;
        }
        const ebase_product_ledger_result_t finished = ebase_product_ledger_finish(
            ledger, &io, request->operation_sequence, request->operation_id,
            command->request.fingerprint, EBASE_PRODUCT_SUCCEEDED, 0U,
            request->expected_container_sequence + 1U);
        finish_product_without_worker(slot, ledger, finished != EBASE_LEDGER_OK,
                                 "succeeded", NULL);
        return;
    }
    if (uninstalled == ESP_BASE_CONTAINER_UNINSTALL_REJECTED) {
        const ebase_product_ledger_result_t finished = ebase_product_ledger_finish(
            ledger, &io, request->operation_sequence, request->operation_id,
            command->request.fingerprint, EBASE_PRODUCT_FAILED, 1U,
            request->expected_container_sequence);
        finish_product_without_worker(slot, ledger, finished != EBASE_LEDGER_OK,
                                 "failed", "product_precondition_conflict");
        return;
    }
    finish_product_without_worker(slot, ledger, true, "unknown", "storage_uncertain");
}

static void handle_command_line(const char *line, size_t length, ebase_command_t *command)
{
    const char *error = ebase_parse_command(line, length, command, protocol_work_alloc);
    if (error) { reply(command->request.request_id, "failed", error, NULL); return; }
    if (command->kind == EBASE_STATUS) {
        status_snapshot_t current = snapshot();
        reply(command->request.request_id, "succeeded", NULL, &current);
        return;
    }
    if (command->kind == EBASE_PRODUCT_STATUS) {
        esp_base_storage_claim_t claim = {0};
        if (s_context.storage_owner == NULL ||
            !esp_base_storage_claim(s_context.storage_owner, &claim)) {
            reply(command->request.request_id, "unknown", "operation_busy", NULL);
            return;
        }
        ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
        if (ledger == NULL) {
            const bool released = esp_base_storage_release(&claim);
            if (!released) s_config_uncertain = true;
            reply(command->request.request_id, "unknown",
                  released ? "resource_failure" : "storage_uncertain", NULL);
            return;
        }
        const ebase_product_ledger_io_t io =
            ebase_product_ledger_nvs_io(s_context.flash_io_owner);
        const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
        esp_base_container_binding_snapshot_t binding = {0};
        esp_base_container_active_product_t active = {0};
        esp_base_container_binding_result_t bound = opened == EBASE_LEDGER_OK ?
            esp_base_container_product_status_snapshot(&claim, &binding, &active) :
            ESP_BASE_CONTAINER_BINDING_OK;
        if (bound == ESP_BASE_CONTAINER_BINDING_OK && active.present && active.is_trial &&
            (!ledger->count || ledger->records[ledger->count - 1U].state != EBASE_PRODUCT_PREPARED ||
             strcmp(active.operation_id, ledger->records[ledger->count - 1U].operation_id) ||
             ledger->records[ledger->count - 1U].kind == EBASE_PRODUCT_UNINSTALL ||
             memcmp(active.package_sha256, ledger->records[ledger->count - 1U].package_sha256, 32)))
            bound = ESP_BASE_CONTAINER_BINDING_UNCERTAIN;
        const bool released = bound == ESP_BASE_CONTAINER_BINDING_UNCERTAIN ? false :
            esp_base_storage_release(&claim);
        if (bound == ESP_BASE_CONTAINER_BINDING_UNCERTAIN || !released)
            s_config_uncertain = true;
        if (opened == EBASE_LEDGER_OK && bound == ESP_BASE_CONTAINER_BINDING_OK && released)
            reply_product_status(command->request.request_id, ledger, &binding, &active);
        free(active.product_version);
        free(ledger);
        if (opened != EBASE_LEDGER_OK || bound != ESP_BASE_CONTAINER_BINDING_OK || !released)
            reply(command->request.request_id, "unknown",
                  !released ? "storage_uncertain" :
                  opened == EBASE_LEDGER_UNINITIALIZED ? "product_ledger_uninitialized" :
                  opened == EBASE_LEDGER_BUSY || bound == ESP_BASE_CONTAINER_BINDING_BUSY ?
                  "operation_busy" : bound == ESP_BASE_CONTAINER_BINDING_RESOURCE_FAILURE ?
                  "resource_failure" : "storage_uncertain", NULL);
        return;
    }
    if (command->kind == EBASE_OTA_RESULT) {
        esp_base_ota_receipt_view_t view;
        const bool active = s_ota_active && !strcmp(s_ota_request.operation_id, command->operation_id);
        const esp_base_ota_receipt_result_t result = esp_base_ota_receipt_query(
            s_context.device_id, command->operation_id, active, &view);
        if (result == ESP_BASE_OTA_RECEIPT_OK) reply_ota_result(command->request.request_id, &view);
        else reply(command->request.request_id, result == ESP_BASE_OTA_RECEIPT_UNSUPPORTED ? "failed" : "unknown",
                   result == ESP_BASE_OTA_RECEIPT_UNSUPPORTED ? "ota_signing_unavailable" :
                   result == ESP_BASE_OTA_RECEIPT_NOT_FOUND ? "ota_operation_not_found" : "storage_uncertain", NULL);
        return;
    }
    if (command->kind == EBASE_PRODUCT_RESULT) {
        size_t slot = 0U;
        if (find_product_run(command->operation_id, &slot)) {
            reply_product_run(command->request.request_id, slot);
            return;
        }
        ebase_product_ledger_t *ledger = protocol_work_alloc(sizeof *ledger);
        if (ledger == NULL) {
            reply(command->request.request_id, "unknown", "resource_failure", NULL);
            return;
        }
        const ebase_product_ledger_io_t io =
            ebase_product_ledger_nvs_io(s_context.flash_io_owner);
        const ebase_product_ledger_result_t opened = ebase_product_ledger_open(ledger, &io);
        ebase_product_record_t record;
        const ebase_product_ledger_result_t found = opened == EBASE_LEDGER_OK ?
            ebase_product_ledger_query(ledger, command->operation_id, &record) : opened;
        free(ledger);
        if (found == EBASE_LEDGER_OK) reply_product_result(command->request.request_id, &record);
        else reply(command->request.request_id, "unknown",
                   (found == EBASE_LEDGER_UNKNOWN || found == EBASE_LEDGER_UNINITIALIZED) ?
                   "product_operation_not_found" :
                   found == EBASE_LEDGER_BUSY ? "operation_busy" : "storage_uncertain", NULL);
        return;
    }
    if (command->kind == EBASE_CONFIG_SET) {
        if (!esp_base_remote_config_with_canonical_bytes(command->config,
                fingerprint_config_bytes, command->request.fingerprint)) {
            reply(command->request.request_id, "failed", "resource_failure", NULL); return;
        }
    }
    if (command->kind == EBASE_PRODUCT_UNINSTALL_COMMAND &&
        !fingerprint_product_uninstall(command->product_uninstall,
                                       command->request.fingerprint)) {
        reply(command->request.request_id, "failed", "resource_failure", NULL);
        return;
    }
    if ((command->kind == EBASE_PRODUCT_STOP_COMMAND ||
         command->kind == EBASE_PRODUCT_START_COMMAND) &&
        !fingerprint_product_run(command, command->request.fingerprint)) {
        reply(command->request.request_id, "failed", "resource_failure", NULL);
        return;
    }
    if ((command->kind == EBASE_PRODUCT_INSTALL_COMMAND ||
         command->kind == EBASE_PRODUCT_UPGRADE_COMMAND) &&
        !esp_base_product_package_source_request_valid(
            command->product_package->package_url,
            command->product_package->package_size_bytes)) {
        reply(command->request.request_id, "failed", "invalid_request", NULL);
        return;
    }
    if ((command->kind == EBASE_PRODUCT_INSTALL_COMMAND ||
         command->kind == EBASE_PRODUCT_UPGRADE_COMMAND) &&
        !fingerprint_product_package(command, command->request.fingerprint)) {
        reply(command->request.request_id, "failed", "resource_failure", NULL);
        return;
    }
    if (command->kind == EBASE_OTA_START) {
        if (!fingerprint_ota_request(command->ota, command->request.fingerprint)) {
            reply(command->request.request_id, "failed", "resource_failure", NULL); return;
        }
    }
    size_t slot = 0;
    const ebase_admission_t decision = ebase_admit(&s_guard, &command->request,
        s_context.device_id, s_boot_id, uptime_ms(), &slot);
    if (decision == EBASE_REPLAY) { emit_outcome(slot, s_reply_mqtt); return; }
    if (decision != EBASE_ACCEPT) {
        reply(command->request.request_id, decision == EBASE_EXPIRED ? "expired" : "failed", admission_error(decision), NULL);
        return;
    }
    s_outcomes[slot].via_mqtt = s_reply_mqtt;
    if (s_frp_restart_pending || s_mqtt_restart_pending) {
        save_outcome(slot, "failed", "operation_busy", false);
        return;
    }
    if (command->kind == EBASE_PRODUCT_STOP_COMMAND ||
        command->kind == EBASE_PRODUCT_START_COMMAND) {
        handle_product_run(slot, command);
        return;
    }
    if (command->kind == EBASE_PRODUCT_INSTALL_COMMAND ||
        command->kind == EBASE_PRODUCT_UPGRADE_COMMAND) {
        handle_product_package(slot, command);
        return;
    }
    if (command->kind == EBASE_PRODUCT_UNINSTALL_COMMAND) {
        handle_product_uninstall(slot, command);
        return;
    }
    if (s_reply_mqtt && command->kind == EBASE_CONFIG_SET) {
        save_outcome(slot, "failed", "physical_usb_required", false);
        return;
    }
    if (command->kind == EBASE_CONFIG_SET) {
        const char *ota_error = esp_base_control_state_config_write_error(&s_control_state);
        if (ota_error != NULL) { save_outcome(slot, "failed", ota_error, false); return; }
    }
    if (command->kind == EBASE_OTA_START) {
        if (!eota_available()) { save_outcome(slot, "failed", "ota_signing_unavailable", false); return; }
        if (command->ota->package_mode == ESP_BASE_OTA_PACKAGE_WRITE &&
            !esp_base_product_package_source_request_valid(
                command->ota->package_url, command->ota->package_size_bytes)) {
            save_outcome(slot, "failed", "invalid_request", false); return;
        }
        eota_image_t candidate = {
            .image_url = command->ota->image_url,
            .image_size_bytes = command->ota->image_size_bytes,
        };
        memcpy(candidate.sha256, command->ota->sha256, sizeof candidate.sha256);
        if (eota_validate_image_request(&candidate) != EOTA_UPDATE_OK) {
            save_outcome(slot, "failed", "invalid_request", false); return;
        }
        if (esp_base_control_state_ota_pending(&s_control_state)) { save_outcome(slot, "failed", "ota_verification_pending", false); return; }
        if (s_ota_active) { save_outcome(slot, "failed", "ota_in_progress", false); return; }
        if (s_product_active) { save_outcome(slot, "failed", "operation_busy", false); return; }
        if (s_trial_active) { save_outcome(slot, "failed", "configuration_busy", false); return; }
        if (s_config_uncertain) { save_outcome(slot, "failed", "storage_uncertain", false); return; }
        if (s_ota_boot_uncertain) { save_outcome(slot, "failed", "ota_boot_state_unknown", false); return; }
        if (!esp_base_container_product_ota_ready(command->ota->package_mode)) {
            save_outcome(slot, "failed", "product_ota_unavailable", false); return;
        }
        if (!esp_base_wifi_ready()) { save_outcome(slot, "failed", "network_unavailable", false); return; }
        if (!esp_base_time_ready()) { save_outcome(slot, "failed", "time_unavailable", false); return; }
        if (!esp_base_storage_claim(s_context.storage_owner, &s_ota_storage_claim)) {
            save_outcome(slot, "failed", "operation_busy", false); return;
        }
        esp_base_ota_receipt_snapshot_t snapshot = {0};
        if (!esp_base_container_product_snapshot_for_ota(
                &s_ota_storage_claim, command->ota, &snapshot)) {
            const bool released = esp_base_storage_release(&s_ota_storage_claim);
            if (!released) s_config_uncertain = true;
            save_outcome(slot, released ? "failed" : "unknown",
                         released ? "product_ota_unavailable" : "storage_uncertain", false);
            return;
        }
        const esp_base_ota_receipt_result_t receipt = esp_base_ota_receipt_register(
            s_context.device_id, command->ota, &snapshot);
        if (receipt != ESP_BASE_OTA_RECEIPT_OK) {
            const char *receipt_error = receipt == ESP_BASE_OTA_RECEIPT_EXISTS ? "ota_operation_exists" :
                receipt == ESP_BASE_OTA_RECEIPT_CONFLICT ? "ota_operation_conflict" :
                receipt == ESP_BASE_OTA_RECEIPT_BUSY ? "ota_previous_unresolved" :
                receipt == ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE ? "ota_slot_unavailable" :
                receipt == ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH ? "ota_selector_mismatch" :
                receipt == ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID ? "ota_source_not_valid" :
                receipt == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE ? "ota_target_not_safe" :
                receipt == ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN ? "ota_target_state_unknown" :
                receipt == ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH ? "ota_snapshot_mismatch" :
                receipt == ESP_BASE_OTA_RECEIPT_SAME_IMAGE ? "ota_same_image" :
                receipt == ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE ? "storage_failure" : "storage_uncertain";
            bool uncertain = receipt == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
            if (!uncertain && !esp_base_storage_release(&s_ota_storage_claim)) uncertain = true;
            if (uncertain) s_config_uncertain = true;
            save_outcome(slot, uncertain ? "unknown" : "failed",
                         uncertain ? "storage_uncertain" : receipt_error, false);
            return;
        }
        s_ota_request = *command->ota;
        s_ota_slot = slot;
        s_ota_active = true;
        atomic_store_explicit(&s_ota_done, false, memory_order_relaxed);
        atomic_store_explicit(&s_ota_stage_uncertain, false, memory_order_relaxed);
        atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
        esp_base_control_state_set_ota_download_active(&s_control_state, true);
        if (xTaskCreate(ota_task, "base_ota", 12288, NULL, 4, NULL) != pdPASS) {
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            memset(&s_ota_request, 0, sizeof s_ota_request);
            if (esp_base_ota_receipt_record_failure(s_context.device_id, command->ota->operation_id,
                    EOTA_UPDATE_RESOURCE_FAILURE) == ESP_BASE_OTA_RECEIPT_OK &&
                esp_base_storage_release(&s_ota_storage_claim)) {
                save_outcome(slot, "failed", "resource_failure", false);
            } else {
                s_config_uncertain = true;
                save_outcome(slot, "unknown", "storage_uncertain", false);
            }
            return;
        }
        save_outcome(slot, "running", NULL, false);
        return;
    }
    const char *write_error = restart_write_error();
    if (write_error) { save_outcome(slot, "failed", write_error, false); return; }
    if (command->kind == EBASE_CONFIG_SET) {
        if (command->config->revision != s_context.config.revision) {
            save_outcome(slot, "failed", "revision_conflict", false); return;
        }
        if (command->config->revision == UINT32_MAX) { save_outcome(slot, "failed", "revision_exhausted", false); return; }
        if (command->config->frp.configured && s_context.frp_flash_store == NULL) {
            save_outcome(slot, "failed", "frp_storage_unavailable", false); return;
        }
        /* Transfer the validated config to its trial owner. The command no
         * longer owns it, and later parsing cannot erase the active trial. */
        s_candidate = command->config;
        command->payload = NULL;
        command->payload_size_bytes = 0U;
        s_trial_slot = slot;
        s_trial_deadline = uptime_ms() + 20000;
        save_outcome(slot, "running", NULL, false);
        if (esp_base_wifi_apply(&s_candidate->wifi, uptime_ms()) != ESP_OK) {
            clear_candidate();
            restore_committed(uptime_ms());
            save_outcome(slot, "failed", "connection_proof_failed", false);
        } else s_trial_active = true;
        return;
    }
    if (s_reply_mqtt) {
        store_outcome(slot, "running", NULL, false);
        const int length = format_result_json(s_response_json, sizeof s_response_json,
            command->request.request_id, "running", NULL, NULL);
        if (length < 0 || !esp_base_mqtt_owner_restart_result(s_response_json, (size_t)length)) {
            save_outcome(slot, "failed", "resource_failure", false);
            return;
        }
        s_mqtt_restart_since_ms = uptime_ms();
        s_mqtt_restart_pending = true;
        return;
    }
    save_outcome(slot, "running", NULL, false);
    (void)fsync(STDOUT_FILENO);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void handle_line(const char *line, size_t length, void *context)
{
    (void)context;
    ebase_command_t *command = protocol_work_alloc(sizeof *command);
    if (command == NULL) {
        reply("", "failed", "resource_failure", NULL);
        return;
    }
    memset(command, 0, sizeof *command);
    handle_command_line(line, length, command);
    ebase_command_release(command);
    free(command);
}

static void handle_mqtt_command(const uint8_t *json, size_t length, void *context)
{
    (void)context;
    s_reply_mqtt = true;
    handle_line((const char *)json, length, NULL);
    s_reply_mqtt = false;
}

static bool handle_mqtt_event(const ebase_mqtt_event_view_t *event, void *context)
{
    (void)context;
    if (event == NULL) return false;
    uint8_t digest[32];
    size_t digest_size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, event->event, event->event_size_bytes,
                         digest, sizeof digest, &digest_size) != PSA_SUCCESS ||
        digest_size != sizeof digest) return false;
    return esp_base_container_product_offer_event(event->package_sha256,
        event->event_sequence, digest, event->event,
        event->event_size_bytes) == ESP_BASE_CONTAINER_EVENT_ACCEPTED;
}

static void feed_serial(const unsigned char *bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const unsigned char c = bytes[i];
        if (s_serial_discard) {
            if (c == '\n') {
                s_serial_discard = false;
                handle_line(NULL, 0, NULL);
            }
            continue;
        }
        if (s_reader == NULL) {
            if (c == '\n') continue;
            s_reader = calloc(1, sizeof *s_reader);
            if (s_reader == NULL) {
                s_serial_discard = true;
                continue;
            }
        }
        ebase_line_feed(s_reader, &c, 1, handle_line, NULL);
        if (c == '\n' || s_reader->discard) {
            const bool discard = s_reader->discard;
            ebase_line_release(s_reader);
            free(s_reader);
            s_reader = NULL;
            /* ebase_line_feed reports an invalid line only at LF. Preserve
             * that contract after releasing the unused oversized buffer. */
            if (discard) s_serial_discard = true;
        }
    }
}

static void expire_serial_input(uint64_t now, uint64_t last_input)
{
    if (s_reader == NULL || !s_reader->length || now - last_input < 2000U) return;
    ebase_line_release(s_reader);
    free(s_reader);
    s_reader = NULL;
    s_serial_discard = true; /* Never interpret a timed-out tail as a command. */
}

static void poll_network_owners(uint64_t now)
{
    const bool pending = esp_base_control_state_ota_pending(&s_control_state);
    const bool business_verification = pending &&
        atomic_load_explicit(&s_firmware_package_verifying, memory_order_acquire);
    if (!pending || business_verification) {
        bool mqtt_can_poll = true;
        if (!s_mqtt_revision_set || s_mqtt_revision != s_context.config.revision) {
            emqtt_config_t *mqtt_work = malloc(sizeof *mqtt_work);
            if (mqtt_work == NULL) {
                /* Revoke the previous revision before retrying setup. Never
                 * poll an old endpoint with the new configuration active. */
                (void)esp_base_mqtt_owner_revoke();
                mqtt_can_poll = false;
            } else {
                (void)esp_base_mqtt_owner_configure(&s_context.config.mqtt, s_context.device_id,
                                                    s_boot_id, mqtt_work);
                memset(mqtt_work, 0, sizeof *mqtt_work);
                free(mqtt_work);
                s_mqtt_revision = s_context.config.revision;
                s_mqtt_revision_set = true;
            }
        }
        if (mqtt_can_poll)
            esp_base_mqtt_owner_poll(now, esp_base_wifi_ready(), esp_base_time_ready(),
                                     handle_mqtt_command, handle_mqtt_event, NULL);
        s_pending_mqtt_active = business_verification;
        if (business_verification) poll_firmware_package_health(now);
        else poll_product_trial_health(now);
    } else if (s_pending_mqtt_active && esp_base_mqtt_owner_revoke() == ESP_OK) {
        s_pending_mqtt_active = false;
        s_mqtt_revision_set = false;
    }
    if (!pending) {
        if (!s_frp_revision_set || s_frp_revision != s_context.config.revision) {
            /* Revoke the old endpoint before waiting for the old FRP worker
             * to finish; no stale management key remains reachable. */
            esp_base_frp_management_listener_configure(NULL);
            const esp_err_t frp_configured = esp_base_frp_owner_configure(
                &s_context.config.frp, s_context.device_id, s_context.frp_flash_store);
            if (frp_configured == ESP_OK) {
                esp_base_frp_management_listener_configure(&s_context.config.frp);
                s_frp_revision = s_context.config.revision;
                s_frp_revision_set = true;
            } else if (frp_configured == ESP_ERR_INVALID_STATE) {
                /* No recovered scratch exists for this boot. No endpoint or
                 * FRP client can start; retry only after a new revision. */
                s_frp_revision = s_context.config.revision;
                s_frp_revision_set = true;
            }
        }
        esp_base_frp_management_listener_poll(now, handle_frp_management, NULL);
        esp_base_frp_owner_poll(now, esp_base_wifi_ready(), esp_base_time_ready(),
                                esp_base_frp_management_listener_ready());
    }
}

static void control_task(void *argument)
{
    (void)argument;
    uint64_t next_report = 0, next_time_poll = 0, last_input = 0;
    unsigned char bytes[256];
    while (true) {
        const uint64_t now = uptime_ms();
        esp_base_wifi_poll(now);
        poll_configuration(now);
        poll_ota();
        poll_product();
        poll_product_trial_failure();
        if (now >= next_time_poll) {
            esp_base_time_poll();
            next_time_poll = now + 1000;
        }
        poll_network_owners(now);
        poll_mqtt_restart(uptime_ms());
        poll_frp_restart(uptime_ms());
        if (now >= next_report) { reported(); next_report = now + 5000; }
        expire_serial_input(now, last_input);
        size_t count = 0;
        // Read from the selected console VFS without blocking the control loop.
        // The C3 USB FIFO backpressures the host; UART0 has no such guarantee
        // and needs its own physical overload check before device acceptance.
        while (count < sizeof bytes && read(STDIN_FILENO, bytes + count, 1) == 1) ++count;
        if (count > 0) {
            last_input = uptime_ms();
            feed_serial(bytes, count);
        }
        esp_base_control_state_note_progress(&s_control_state);
        vTaskDelay(1); /* Let idle/WDT and other capabilities run under sustained input. */
    }
}

esp_err_t esp_base_protocol_load_config(uint32_t *revision,
                                        esp_base_storage_owner_t *flash_io_owner)
{
    if (!revision || flash_io_owner == NULL) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_config_loaded = false;
    /* RTC may survive deep sleep. Never treat prior RAM as committed config. */
    memset(&s_context, 0, sizeof s_context);
    const esp_err_t error = load_config_with_flash_io(
        &s_context.config, flash_io_owner);
    if (error != ESP_OK) return error;
    *revision = s_context.config.revision;
    s_context.flash_io_owner = flash_io_owner;
    s_config_loaded = true;
    return ESP_OK;
}

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{
    if (!context || !ebase_is_uuid(context->device_id) || context->storage_owner == NULL ||
        context->flash_io_owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started || !s_config_loaded ||
        context->flash_io_owner != s_context.flash_io_owner) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_base_identity_generate_uuid(s_boot_id, sizeof s_boot_id);
    if (error != ESP_OK) return error;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    usb_serial_jtag_vfs_use_nonblocking();
    // IDF 6.1 USB no-driver VFS prefetches with O_NONBLOCK clear.
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0) return ESP_FAIL;
#else
    uart_vfs_dev_use_nonblocking(CONFIG_ESP_CONSOLE_UART_NUM);
    // UART0 has no USB packet backpressure; keep reads nonblocking so the
    // control loop continues OTA progress and network owner polling.
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return ESP_FAIL;
#endif
    /* Config was loaded into s_context before starting this task. Copy only
     * immutable metadata; replacing the whole context would erase it. */
    s_context.device_id = context->device_id;
    s_context.firmware_version = context->firmware_version;
    s_context.chip_model = context->chip_model;
    s_context.flash_size_bytes = context->flash_size_bytes;
    s_context.reset_reason = context->reset_reason;
    s_context.storage_owner = context->storage_owner;
    s_context.flash_io_owner = context->flash_io_owner;
    s_context.frp_flash_store = context->frp_flash_store;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    error = esp_base_wifi_start(&s_context.config.wifi);
    if (error != ESP_OK) {
        ESP_LOGW("base_wifi", "ESP_BASE_WIFI_UNAVAILABLE error=%s", esp_err_to_name(error));
    }
    /* Signed C3 joint OTA reached only 352 free stack bytes at 6144.
     * Both targets reserve 8192 for the complete SDK verification call path. */
    const uint32_t control_stack_bytes = 8192;
    if (xTaskCreate(control_task, "base_control", control_stack_bytes, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool esp_base_protocol_control_healthy(void)
{
    return s_started && esp_base_control_state_is_recent(&s_control_state);
}

uint32_t esp_base_protocol_control_progress_count(void)
{
    return esp_base_control_state_progress_count(&s_control_state);
}

bool esp_base_protocol_begin_firmware_package_verification(
    const esp_base_storage_claim_t *claim, const uint8_t package_sha256[32])
{
    if (!s_started || !esp_base_storage_claim_active(claim) ||
        claim->owner != s_context.storage_owner || package_sha256 == NULL ||
        !esp_base_control_state_ota_pending(&s_control_state) ||
        !esp_base_container_product_event_accepting() ||
        atomic_load_explicit(&s_firmware_package_verifying, memory_order_acquire) ||
        atomic_flag_test_and_set_explicit(&s_firmware_health_lock, memory_order_acquire))
        return false;
    memset(&s_firmware_health, 0, sizeof s_firmware_health);
    memcpy(s_firmware_health.package_sha256, package_sha256, 32);
    atomic_store_explicit(&s_firmware_package_verifying, true, memory_order_release);
    atomic_flag_clear_explicit(&s_firmware_health_lock, memory_order_release);
    return true;
}

bool esp_base_protocol_firmware_package_health_snapshot(
    esp_base_protocol_firmware_package_health_t *out)
{
    if (out == NULL) return false;
    *out = (esp_base_protocol_firmware_package_health_t){0};
    if (!atomic_load_explicit(&s_firmware_package_verifying, memory_order_acquire) ||
        !esp_base_control_state_ota_pending(&s_control_state) ||
        !esp_base_protocol_control_healthy() ||
        atomic_flag_test_and_set_explicit(&s_firmware_health_lock, memory_order_acquire))
        return false;
    const uint64_t now = uptime_ms();
    const bool ready = s_firmware_health.ready &&
        now >= s_firmware_health.last_poll_ms &&
        now - s_firmware_health.last_poll_ms <= PRODUCT_TRIAL_PROGRESS_GAP_MS;
    if (ready) {
        out->event_sequence = s_firmware_health.event_sequence;
        out->failure_count = s_firmware_health.failure_count;
    }
    atomic_flag_clear_explicit(&s_firmware_health_lock, memory_order_release);
    return ready;
}

void esp_base_protocol_end_firmware_package_verification(void)
{
    atomic_store_explicit(&s_firmware_package_verifying, false, memory_order_release);
}

void esp_base_protocol_set_ota_verification_pending(bool pending)
{
    esp_base_control_state_set_ota_pending(&s_control_state, pending);
}
