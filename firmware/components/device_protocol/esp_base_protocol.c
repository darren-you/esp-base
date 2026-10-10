// SPDX-License-Identifier: Apache-2.0
#include "esp_base_protocol.h"
#include "esp_base_capacity.h"
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
#include "esp_base_business.h"
#include "esp_base_ota_firmware.h"
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
static ebase_business_t s_business;
static size_t s_ota_slot;
/* The accepted command transfers its existing request to the control owner.
 * The worker borrows it until publishing completion; queries never replace it. */
static esp_base_ota_request_t *s_ota_request;
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
static char *s_frp_reply;
static size_t s_frp_reply_capacity, s_frp_reply_length;
static bool s_parse_frp;
static ebase_command_kind_t s_frp_expected_kind;
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
    bool has_status, via_mqtt;
    status_snapshot_t status;
} command_outcome_t;
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

static bool fingerprint_ota_request(const esp_base_ota_request_t *request, uint8_t fingerprint[32])
{
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    size_t written = 0;
    const uint8_t source = request->inbound_stream ? 1U : 0U;
    uint8_t size[4];
    for (size_t i = 0; i < 4; ++i) size[i] = (uint8_t)(request->image_size_bytes >> ((3U-i)*8U));
    bool valid = psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)"ota.start", 10) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->operation_id, sizeof request->operation_id) == PSA_SUCCESS &&
        psa_hash_update(&hash, (const uint8_t *)request->image_url, strlen(request->image_url)+1U) == PSA_SUCCESS &&
        psa_hash_update(&hash, request->sha256, 32) == PSA_SUCCESS &&
        psa_hash_update(&hash, size, sizeof size) == PSA_SUCCESS &&
        psa_hash_update(&hash, &source, 1) == PSA_SUCCESS &&
        psa_hash_finish(&hash, fingerprint, 32, &written) == PSA_SUCCESS && written == 32;
    if (!valid) (void)psa_hash_abort(&hash);
    return valid;
}

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void digest_hex(char output[67], const uint8_t digest[32])
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
        s_ota_active ? s_ota_request->image_size_bytes : 0);
    char digest[67] = "null", completed[24] = "null", business_result[16] = "null";
    if (s_business.last_event_sequence) {
        (void)snprintf(completed, sizeof completed, "%" PRIu64, s_business.last_event_sequence);
        (void)snprintf(business_result, sizeof business_result, "%" PRId32, s_business.last_result);
    }
    if (s_business.last_event_sequence) digest_hex(digest, s_business.last_event_sha256);
    const int size = snprintf(s_response_json, MQTT_REPORTED_JSON_BYTES,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"uptime_ms\":%" PRIu64 ",\"revision\":%" PRIu32 ","
        "\"wifi_state\":\"%s\",\"time_ready\":%s,\"frp_state\":\"%s\","
        "\"last_accepted_event_sequence\":%" PRIu64 ","
        "\"last_completed_event_sequence\":%s,\"last_completed_event_sha256\":%s,"
        "\"last_event_outcome\":\"%s\",\"last_business_result\":%s}",
        s_context.device_id, s_boot_id, now, s_context.config.revision,
        esp_base_wifi_state(), time_ready ? "true" : "false", frp.state,
        esp_base_mqtt_owner_event_sequence(), completed, digest,
        !s_business.last_event_sequence ? "none" : s_business.last_result < 0 ? "business_failed" : "succeeded",
        business_result);
    if (size > 0 && (size_t)size < MQTT_REPORTED_JSON_BYTES)
        (void)esp_base_mqtt_owner_reported(s_response_json, (size_t)size);
}

static status_snapshot_t snapshot(void)
{
    return (status_snapshot_t){.uptime = uptime_ms(), .revision = s_context.config.revision,
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
        .ota_received = (uint32_t)atomic_load_explicit(&s_ota_received, memory_order_relaxed),
        .ota_total = s_ota_active ? s_ota_request->image_size_bytes : 0,
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

static void emit_json(int length)
{
    if (length < 0 || (size_t)length >= sizeof s_response_json) return;
    if (s_frp_reply) {
        if ((size_t)length >= s_frp_reply_capacity) return;
        memcpy(s_frp_reply, s_response_json, (size_t)length);
        s_frp_reply_length = (size_t)length;
    } else if (s_reply_mqtt) {
        (void)esp_base_mqtt_owner_result(s_response_json, (size_t)length);
    } else {
        flockfile(stdout);
        fputc('\n', stdout); (void)fwrite(s_response_json, 1, (size_t)length, stdout);
        fputc('\n', stdout); fflush(stdout); funlockfile(stdout);
    }
}

static void reply(const char *request_id, const char *state, const char *error, const status_snapshot_t *status)
{
    emit_json(format_result_json(s_response_json, sizeof s_response_json,
        request_id && request_id[0] ? request_id : NULL, state, error, status));
}

static void reply_ota_result(const char *request_id, const esp_base_ota_receipt_view_t *view)
{
    const char *state = view->state == ESP_BASE_OTA_OPERATION_RUNNING ? "running" :
        view->state == ESP_BASE_OTA_OPERATION_SUCCEEDED ? "succeeded" :
        view->state == ESP_BASE_OTA_OPERATION_FAILED ? "failed" : "unknown";
    char digest[67]; digest_hex(digest, view->sha256);
    emit_json(snprintf(s_response_json, sizeof s_response_json,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":%s%s%s,"
        "\"result\":{\"operation_id\":\"%s\",\"sha256\":%s,"
        "\"image_size_bytes\":%" PRIu32 ",\"target\":\"%s\",\"target_slot\":\"%s\"}}",
        s_context.device_id, s_boot_id, request_id, state,
        view->error_code ? "\"" : "null", view->error_code ? view->error_code : "",
        view->error_code ? "\"" : "", view->operation_id, digest, view->image_size_bytes, ESP_BASE_OTA_TARGET,
        view->target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1"));
}

static void reply_business(const char *request_id)
{
    ebase_business_poll(&s_business, uptime_ms());
    emit_json(snprintf(s_response_json, sizeof s_response_json,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":\"%s\",\"state\":\"succeeded\",\"error_code\":null,"
        "\"result\":{\"byte_count\":%" PRIu32 ",\"state\":\"%s\",\"window_deadline_uptime_ms\":%" PRIu64 "}}",
        s_context.device_id, s_boot_id, request_id, s_business.byte_count,
        s_business.state == EBASE_BUSINESS_IDLE ? "idle" : s_business.state == EBASE_BUSINESS_ACTIVE ? "active" : "paused",
        s_business.window_deadline_ms));
}

static void reply_firmware(const char *request_id)
{
    esp_base_storage_claim_t claim = {0};
    if (!esp_base_storage_claim(s_context.storage_owner, &claim)) {
        reply(request_id, "unknown", "operation_busy", NULL); return;
    }
    const eota_policy_t policy = esp_base_ota_policy(true);
    eota_slots_t slots = {0}, after = {0}; uint32_t size = 0; uint8_t digest[32];
    bool valid = eota_observe_slots(&policy, &slots) == EOTA_UPDATE_OK &&
        slots.running_subtype == slots.boot_subtype &&
        eota_sha256_verified_image(&policy, slots.running_subtype, &size, digest) == EOTA_UPDATE_OK &&
        eota_observe_slots(&policy, &after) == EOTA_UPDATE_OK &&
        after.running_subtype == slots.running_subtype && after.boot_subtype == slots.boot_subtype &&
        after.running_state == slots.running_state;
    if (!esp_base_storage_release(&claim)) { valid = false; s_config_uncertain = true; }
    if (!valid) { reply(request_id, "unknown", "storage_uncertain", NULL); return; }
    char hex[67]; digest_hex(hex, digest);
    emit_json(snprintf(s_response_json, sizeof s_response_json,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":\"%s\",\"state\":\"succeeded\",\"error_code\":null,"
        "\"result\":{\"firmware_sha256\":%s,\"image_size_bytes\":%" PRIu32 ",\"target\":\"%s\",\"ota_slot\":\"%s\"}}",
        s_context.device_id, s_boot_id, request_id, hex, size, ESP_BASE_OTA_TARGET,
        slots.running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1"));
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
    const bool via_mqtt = s_outcomes[slot].via_mqtt;
    s_outcomes[slot] = (command_outcome_t){.state = state, .error = error,
        .has_status = status, .via_mqtt = via_mqtt};
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
    if (s_frp_restart_pending || s_mqtt_restart_pending) return "operation_busy";
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

static void handle_line(const char *line, size_t length, void *context);

static int handle_frp_management(esp_base_frp_management_command_t command,
                                 const uint8_t *json, size_t json_length,
                                 char *response, size_t capacity,
                                 size_t *response_length, void *context)
{
    if (command == ESP_BASE_FRP_MANAGEMENT_STATUS)
        return handle_frp_status(json, json_length, response, capacity, response_length, context);
    if (command == ESP_BASE_FRP_MANAGEMENT_RESTART)
        return handle_frp_restart(json, json_length, response, capacity, response_length);
    switch (command) {
    case ESP_BASE_FRP_MANAGEMENT_FIRMWARE_STATUS: s_frp_expected_kind = EBASE_FIRMWARE_STATUS; break;
    case ESP_BASE_FRP_MANAGEMENT_OTA_START: s_frp_expected_kind = EBASE_OTA_START; break;
    case ESP_BASE_FRP_MANAGEMENT_OTA_RESULT: s_frp_expected_kind = EBASE_OTA_RESULT; break;
    case ESP_BASE_FRP_MANAGEMENT_BUSINESS_STATUS: s_frp_expected_kind = EBASE_BUSINESS_STATUS; break;
    case ESP_BASE_FRP_MANAGEMENT_BUSINESS_PAUSE: s_frp_expected_kind = EBASE_BUSINESS_PAUSE; break;
    case ESP_BASE_FRP_MANAGEMENT_BUSINESS_RESUME: s_frp_expected_kind = EBASE_BUSINESS_RESUME; break;
    default: return 500;
    }
    s_frp_reply = response; s_frp_reply_capacity = capacity; s_frp_reply_length = 0;
    s_parse_frp = true;
    handle_line((const char *)json, json_length, NULL);
    s_parse_frp = false; s_frp_reply = NULL;
    *response_length = s_frp_reply_length;
    if (*response_length < capacity) response[*response_length] = '\0';
    if (!*response_length) return 500;
    if (strstr(response, "\"error_code\":\"invalid_request\"")) return 400;
    if (strstr(response, "\"state\":\"failed\"") ||
        strstr(response, "\"state\":\"expired\"")) return 409;
    return strstr(response, "\"state\":\"running\"") ? 202 : 200;
}

static void poll_frp_restart(uint64_t now)
{
    if (!s_frp_restart_pending || now - s_frp_restart_since_ms < 100U) return;
    /* Allow the authenticated receipt to drain, with a hard two-second bound
     * even if the peer stops reading. Receipt loss remains unconfirmed. */
    if (esp_base_frp_management_listener_response_pending() &&
        now - s_frp_restart_since_ms < 2000U) return;
    s_frp_restart_pending = false;
    esp_base_capacity_before_reset(s_boot_id, uptime_ms());
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
    esp_base_capacity_before_reset(s_boot_id, uptime_ms());
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

static void clear_ota_request(void)
{
    if (s_ota_request == NULL) return;
    volatile unsigned char *bytes = (volatile unsigned char *)s_ota_request;
    for (size_t index = 0; index < sizeof *s_ota_request; ++index) bytes[index] = 0U;
    free(s_ota_request);
    s_ota_request = NULL;
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
    const eota_policy_t policy = esp_base_ota_policy(true);
    esp_base_ota_receipt_recovery_t receipt = {0};
    eota_result_t result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    bool mutated = false;
    const char *stage = "receipt_load";
    bool upload_finished = false;
    if (esp_base_ota_receipt_load_for_recovery(s_context.device_id, &receipt) != ESP_BASE_OTA_RECEIPT_OK ||
        receipt.status != ESP_BASE_OTA_RECEIPT_PREPARED ||
        strcmp(receipt.operation_id, s_ota_request->operation_id) ||
        receipt.image_size_bytes != s_ota_request->image_size_bytes ||
        memcmp(receipt.candidate_sha256, s_ota_request->sha256, 32)) goto uncertain;
    if (s_ota_request->inbound_stream) {
        stage = "wait_upload";
        const uint64_t started = uptime_ms();
        while (!esp_base_frp_management_upload_connected()) {
            const uint64_t now = uptime_ms();
            if (now < started || now - started >= policy.connect_timeout_ms) {
                result = EOTA_UPDATE_DOWNLOAD_FAILED; goto done;
            }
            vTaskDelay(1);
        }
    }
    stage = "retire_inactive";
    result = eota_retire_inactive(&policy, receipt.target_subtype, receipt.source_sha256);
    if (result != EOTA_UPDATE_OK) goto uncertain;
    mutated = true;
    eota_image_t image = {.image_url = s_ota_request->image_url,
        .image_size_bytes = s_ota_request->image_size_bytes};
    memcpy(image.sha256, s_ota_request->sha256, 32);
    eota_prepared_t prepared = {0};
    stage = "prepare";
    if (s_ota_request->inbound_stream) {
        eota_stream_t stream = {.read = esp_base_frp_management_upload_read,
            .image_size_bytes = image.image_size_bytes};
        memcpy(stream.sha256, image.sha256, 32);
        result = eota_prepare_stream(&policy, &stream, ota_progress, NULL, &prepared);
    } else result = eota_prepare(&policy, &image, ota_progress, NULL, &prepared);
    if (result == EOTA_UPDATE_OK && s_ota_request->inbound_stream) {
        stage = "upload_connection";
        if (!esp_base_frp_management_upload_connected()) result = EOTA_UPDATE_DOWNLOAD_FAILED;
    }
    if (result == EOTA_UPDATE_OK) {
        stage = "select";
        result = eota_select(&policy, &prepared);
    }
    if (result != EOTA_UPDATE_OK) {
        /* A receipt-bound cleanup must prove A remains VALID/selected and C's
         * first sector is erased before marking any partial write failed. */
        if (result == EOTA_UPDATE_BOOT_STATE_UNKNOWN ||
            (mutated && eota_retire_inactive(&policy, receipt.target_subtype,
                                            receipt.source_sha256) != EOTA_UPDATE_OK)) goto uncertain;
    }
    goto done;
uncertain:
    atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
done:
    if (s_ota_request->inbound_stream) {
        char response[512];
        /* The control owner persists and reads back a failure after this worker
         * finishes. Upload preparation cannot establish a durable terminal state. */
        const bool prepared = result == EOTA_UPDATE_OK &&
            !atomic_load_explicit(&s_ota_stage_uncertain, memory_order_relaxed);
        const int length = format_result_json(response, sizeof response,
            s_guard.entries[s_ota_slot].request_id,
            prepared ? "running" : "unknown", prepared ? NULL : "storage_uncertain", NULL);
        upload_finished = esp_base_frp_management_upload_finish(prepared ? 202 : 200,
            length > 0 ? response : NULL, length > 0 ? (size_t)length : 0, 1000);
    }
    if (result != EOTA_UPDATE_OK) {
        ESP_LOGE("base_ota", "ESP_BASE_OTA_FAILURE operation_id=%s stage=%s error=%s consumed_bytes=%u upload_finished=%s uncertain=%s",
            s_ota_request->operation_id, stage, eota_error(result),
            (unsigned)atomic_load_explicit(&s_ota_received, memory_order_relaxed),
            upload_finished ? "true" : "false",
            atomic_load_explicit(&s_ota_stage_uncertain, memory_order_relaxed) ? "true" : "false");
    }
    /* Host log fakes discard variadic arguments; production logs use both. */
    (void)stage;
    (void)upload_finished;
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
        clear_ota_request();
        return;
    }
    if (result == EOTA_UPDATE_OK) {
        /* The slot is selected but not yet confirmed. A new boot must pass the
         * local self-test and stability window before it becomes valid. */
        save_outcome(s_ota_slot, "running", NULL, false);
        (void)fsync(STDOUT_FILENO);
        esp_base_capacity_before_reset(s_boot_id, uptime_ms());
        esp_restart();
        return;
    }
    if (result == EOTA_UPDATE_BOOT_STATE_UNKNOWN) {
        s_ota_boot_uncertain = true;
        ESP_LOGE("base_ota", "ESP_BASE_OTA_RECOVERY_REQUIRED selector readback unavailable; avoid resetting device");
    } else {
        const esp_base_ota_receipt_result_t saved = esp_base_ota_receipt_record_failure(
            s_context.device_id, s_ota_request->operation_id, result);
        if (saved != ESP_BASE_OTA_RECEIPT_OK) {
            s_config_uncertain = true;
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
            save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
            clear_ota_request();
            return;
        }
        if (!esp_base_storage_release(&s_ota_storage_claim)) {
            s_ota_boot_uncertain = true;
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            atomic_store_explicit(&s_ota_done, false, memory_order_relaxed);
            save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
            clear_ota_request();
            return;
        }
    }
    esp_base_control_state_set_ota_download_active(&s_control_state, false);
    s_ota_active = false;
    atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
    save_outcome(s_ota_slot, result == EOTA_UPDATE_BOOT_STATE_UNKNOWN ? "unknown" : "failed",
                 eota_error(result), false);
    clear_ota_request();
}

static void handle_command_line(const char *line, size_t length, ebase_command_t *command)
{
    const char *error = s_parse_frp ? ebase_parse_frp_command(line, length, command, protocol_work_alloc) :
        ebase_parse_command(line, length, command, protocol_work_alloc);
    if (error) { reply(command->request.request_id, "failed", error, NULL); return; }
    if (s_parse_frp && command->kind != s_frp_expected_kind) {
        reply(command->request.request_id, "failed", "invalid_request", NULL); return;
    }
    if (s_parse_frp && strcmp(command->request.device_id, s_context.device_id)) {
        reply(command->request.request_id, "failed", "wrong_device", NULL); return;
    }
    if (command->kind == EBASE_STATUS) {
        status_snapshot_t current = snapshot(); reply(command->request.request_id, "succeeded", NULL, &current); return;
    }
    if (command->kind == EBASE_FIRMWARE_STATUS) { reply_firmware(command->request.request_id); return; }
    if (command->kind == EBASE_BUSINESS_STATUS) { reply_business(command->request.request_id); return; }
    if (command->kind == EBASE_OTA_RESULT) {
        esp_base_ota_receipt_view_t view;
        const bool active = s_ota_active && !strcmp(s_ota_request->operation_id, command->operation_id);
        const esp_base_ota_receipt_result_t result = esp_base_ota_receipt_query(s_context.device_id, command->operation_id, active, &view);
        if (result == ESP_BASE_OTA_RECEIPT_OK) reply_ota_result(command->request.request_id, &view);
        else reply(command->request.request_id, result == ESP_BASE_OTA_RECEIPT_UNSUPPORTED ? "failed" : "unknown",
            result == ESP_BASE_OTA_RECEIPT_UNSUPPORTED ? "ota_signing_unavailable" :
            result == ESP_BASE_OTA_RECEIPT_NOT_FOUND ? "ota_operation_not_found" : "storage_uncertain", NULL);
        return;
    }
    if (command->kind == EBASE_CONFIG_SET && !esp_base_remote_config_with_canonical_bytes(command->config,
            fingerprint_config_bytes, command->request.fingerprint)) {
        reply(command->request.request_id, "failed", "resource_failure", NULL); return;
    }
    if (command->kind == EBASE_OTA_START && !fingerprint_ota_request(command->ota, command->request.fingerprint)) {
        reply(command->request.request_id, "failed", "resource_failure", NULL); return;
    }
    if (command->kind == EBASE_BUSINESS_PAUSE || command->kind == EBASE_BUSINESS_RESUME) {
        size_t written = 0;
        const char *name = command->kind == EBASE_BUSINESS_PAUSE ? "business.pause" : "business.resume";
        if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)name, strlen(name), command->request.fingerprint, 32, &written) != PSA_SUCCESS || written != 32) {
            reply(command->request.request_id, "failed", "resource_failure", NULL); return;
        }
    }
    size_t slot = 0;
    const ebase_admission_t decision = ebase_admit(&s_guard, &command->request,
        s_context.device_id, s_boot_id, uptime_ms(), &slot);
    if (decision == EBASE_REPLAY) { emit_outcome(slot, s_reply_mqtt); return; }
    if (decision != EBASE_ACCEPT) {
        reply(command->request.request_id, decision == EBASE_EXPIRED ? "expired" : "failed", admission_error(decision), NULL); return;
    }
    s_outcomes[slot].via_mqtt = s_reply_mqtt;
    if (command->kind == EBASE_BUSINESS_PAUSE || command->kind == EBASE_BUSINESS_RESUME) {
        const uint8_t action = command->kind == EBASE_BUSINESS_PAUSE ? 2 : 3;
        const int32_t value = ebase_business_event(&s_business, &action, 1, uptime_ms());
        save_outcome(slot, value < 0 ? "failed" : "succeeded", value < 0 ? "business_failed" : NULL, false); return;
    }
    if (s_frp_restart_pending || s_mqtt_restart_pending) { save_outcome(slot, "failed", "operation_busy", false); return; }
    if ((s_reply_mqtt || s_parse_frp) && command->kind == EBASE_CONFIG_SET) {
        save_outcome(slot, "failed", "physical_usb_required", false); return;
    }
    if (command->kind == EBASE_CONFIG_SET) {
        const char *ota_error = esp_base_control_state_config_write_error(&s_control_state);
        if (ota_error) { save_outcome(slot, "failed", ota_error, false); return; }
    }
    if (command->kind == EBASE_OTA_START) {
        if (!eota_available()) { save_outcome(slot, "failed", "ota_signing_unavailable", false); return; }
        eota_image_t candidate = {
            .image_url = command->ota->image_url,
            .image_size_bytes = command->ota->image_size_bytes,
        };
        memcpy(candidate.sha256, command->ota->sha256, sizeof candidate.sha256);
        const eota_stream_t input = {.read = esp_base_frp_management_upload_read,
            .image_size_bytes = candidate.image_size_bytes};
        eota_stream_t stream_request = input;
        memcpy(stream_request.sha256, candidate.sha256, 32);
        const eota_result_t static_valid = command->ota->inbound_stream ?
            eota_validate_stream_request(&stream_request) : eota_validate_image_request(&candidate);
        if (static_valid != EOTA_UPDATE_OK) {
            save_outcome(slot, "failed", "invalid_request", false); return;
        }
        if (esp_base_control_state_ota_pending(&s_control_state)) { save_outcome(slot, "failed", "ota_verification_pending", false); return; }
        if (s_ota_active) { save_outcome(slot, "failed", "ota_in_progress", false); return; }
        if (s_trial_active) { save_outcome(slot, "failed", "configuration_busy", false); return; }
        if (s_config_uncertain) { save_outcome(slot, "failed", "storage_uncertain", false); return; }
        if (s_ota_boot_uncertain) { save_outcome(slot, "failed", "ota_boot_state_unknown", false); return; }
        if (!esp_base_wifi_ready()) { save_outcome(slot, "failed", "network_unavailable", false); return; }
        if (!esp_base_time_ready()) { save_outcome(slot, "failed", "time_unavailable", false); return; }
        if (!esp_base_storage_claim(s_context.storage_owner, &s_ota_storage_claim)) {
            save_outcome(slot, "failed", "operation_busy", false); return;
        }
        const uint64_t registration_started_ms = uptime_ms();
        const esp_base_ota_receipt_result_t receipt = esp_base_ota_receipt_register(
            s_context.device_id, command->ota, &s_ota_storage_claim);
        ESP_LOGI("base_ota", "ESP_BASE_OTA_REGISTRATION operation_id=%s elapsed_ms=%" PRIu64 " receipt=%u",
            command->ota->operation_id, uptime_ms() - registration_started_ms, (unsigned)receipt);
        (void)registration_started_ms;
        if (receipt != ESP_BASE_OTA_RECEIPT_OK) {
            const char *receipt_error = receipt == ESP_BASE_OTA_RECEIPT_EXISTS ? "ota_operation_exists" :
                receipt == ESP_BASE_OTA_RECEIPT_CONFLICT ? "ota_operation_conflict" :
                receipt == ESP_BASE_OTA_RECEIPT_BUSY ? "ota_previous_unresolved" :
                receipt == ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE ? "ota_slot_unavailable" :
                receipt == ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH ? "ota_selector_mismatch" :
                receipt == ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID ? "ota_source_not_valid" :
                receipt == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE ? "ota_target_not_safe" :
                receipt == ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN ? "ota_target_state_unknown" :
                receipt == ESP_BASE_OTA_RECEIPT_SAME_IMAGE ? "ota_same_image" :
                receipt == ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE ? "storage_failure" : "storage_uncertain";
            bool uncertain = receipt == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
            if (!uncertain && !esp_base_storage_release(&s_ota_storage_claim)) uncertain = true;
            if (uncertain) s_config_uncertain = true;
            save_outcome(slot, uncertain ? "unknown" : "failed",
                         uncertain ? "storage_uncertain" : receipt_error, false);
            return;
        }
        s_ota_request = command->ota;
        command->payload = NULL;
        command->payload_size_bytes = 0U;
        if (s_ota_request->inbound_stream && !esp_base_frp_management_upload_arm(
                s_ota_request->operation_id, s_context.device_id, s_boot_id,
                s_ota_request->image_size_bytes, s_ota_request->sha256, uptime_ms())) {
            const bool failed = esp_base_ota_receipt_record_failure(s_context.device_id,
                s_ota_request->operation_id, EOTA_UPDATE_RESOURCE_FAILURE) == ESP_BASE_OTA_RECEIPT_OK &&
                esp_base_storage_release(&s_ota_storage_claim);
            clear_ota_request();
            if (!failed) s_config_uncertain = true;
            save_outcome(slot, failed ? "failed" : "unknown", failed ? "resource_failure" : "storage_uncertain", false); return;
        }
        s_ota_slot = slot;
        s_ota_active = true;
        atomic_store_explicit(&s_ota_done, false, memory_order_relaxed);
        atomic_store_explicit(&s_ota_stage_uncertain, false, memory_order_relaxed);
        atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
        esp_base_control_state_set_ota_download_active(&s_control_state, true);
        if (xTaskCreate(ota_task, "base_ota", 12288, NULL, 4, NULL) != pdPASS) {
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            const bool failed = esp_base_ota_receipt_record_failure(
                s_context.device_id, s_ota_request->operation_id,
                EOTA_UPDATE_RESOURCE_FAILURE) == ESP_BASE_OTA_RECEIPT_OK &&
                esp_base_storage_release(&s_ota_storage_claim);
            if (s_ota_request->inbound_stream) (void)esp_base_frp_management_upload_finish(500, NULL, 0, 1000);
            clear_ota_request();
            if (failed) {
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
    esp_base_capacity_before_reset(s_boot_id, uptime_ms());
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
    if (!event) return false;
    uint8_t digest[32]; size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, event->event, event->event_size_bytes,
                         digest, 32, &written) != PSA_SUCCESS || written != 32) return false;
    s_business.last_result = ebase_business_event(&s_business, event->event,
                                                  event->event_size_bytes, uptime_ms());
    s_business.last_event_sequence = event->event_sequence;
    memcpy(s_business.last_event_sha256, digest, 32);
    return true;
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
    if (!pending) {
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
        ebase_business_poll(&s_business, now);
        if (now >= next_time_poll) {
            esp_base_time_poll();
            next_time_poll = now + 1000;
        }
        poll_network_owners(now);
        poll_mqtt_restart(uptime_ms());
        poll_frp_restart(uptime_ms());
        esp_base_capacity_poll(s_boot_id, uptime_ms());
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
    ebase_business_reset(&s_business);
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

void esp_base_protocol_set_ota_verification_pending(bool pending)
{
    esp_base_control_state_set_ota_pending(&s_control_state, pending);
}
