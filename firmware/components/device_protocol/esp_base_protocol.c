// SPDX-License-Identifier: Apache-2.0
#include "esp_base_protocol.h"
#include "esp_base_command.h"
#include "esp_base_identity.h"
#include "esp_base_remote_config.h"
#include "esp_base_wifi.h"
#include "psa/crypto.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_base_protocol_context_t s_context;
static char s_boot_id[EBASE_ID_BYTES];
static ebase_request_guard_t s_guard;
static ebase_line_reader_t s_reader;
static bool s_started, s_config_uncertain, s_trial_active;
static size_t s_trial_slot;
static uint64_t s_trial_deadline;
static esp_base_remote_config_t s_candidate;
typedef struct {
    uint64_t uptime;
    uint32_t revision, free_heap, minimum_free_heap;
    const char *wifi, *config;
} status_snapshot_t;
typedef struct {
    const char *state, *error;
    bool has_status;
    status_snapshot_t status;
} command_outcome_t;
static command_outcome_t s_outcomes[EBASE_REQUEST_SLOTS];

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void reported(void)
{
    ESP_LOGI("base_reported",
        "ESP_BASE_REPORTED schema=1 boot_id=%s uptime_ms=%" PRIu64
        " free_heap=%" PRIu32 " min_free_heap=%" PRIu32
        " device_id=%s firmware=%s chip=%s flash=%" PRIu32 " config_generation=%" PRIu32
        " reset=%s provisioned=%s wifi_state=%s mqtt_state=unsupported frp_state=unsupported",
        s_boot_id, uptime_ms(), esp_get_free_heap_size(),
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT), s_context.device_id,
        s_context.firmware_version, s_context.chip_model, s_context.flash_size_bytes,
        s_context.config.revision, s_context.reset_reason,
        s_context.config.wifi.configured ? "true" : "false", esp_base_wifi_state());
}

static status_snapshot_t snapshot(void)
{
    return (status_snapshot_t){.uptime = uptime_ms(), .revision = s_context.config.revision,
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
        .wifi = esp_base_wifi_state(), .config = s_config_uncertain ? "failed" : "ready"};
}

static void reply(const char *request_id, const char *state, const char *error, const status_snapshot_t *status)
{
    /* Strings are validated UUIDs or closed firmware constants. Never echo
     * request text or secrets. One FILE lock covers the complete JSON line. */
    flockfile(stdout);
    printf("\n{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"request_id\":",
           s_context.device_id, s_boot_id);
    if (request_id && request_id[0]) printf("\"%s\"", request_id); else printf("null");
    printf(",\"state\":\"%s\",\"error_code\":", state);
    if (error) printf("\"%s\"", error); else printf("null");
    printf(",\"result\":");
    if (status) {
        printf("{\"uptime_ms\":%" PRIu64 ",\"revision\":%" PRIu32
               ",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32
               ",\"capabilities\":{\"wifi\":\"%s\",\"mqtt\":\"unsupported\","
               "\"frp\":\"unsupported\",\"config\":\"%s\",\"ota\":\"unsupported\"}}",
               status->uptime, status->revision, status->free_heap, status->minimum_free_heap,
               status->wifi, status->config);
    } else printf("null");
    printf("}\n");
    fflush(stdout);
    funlockfile(stdout);
}

static void emit_outcome(size_t slot)
{
    command_outcome_t *out = &s_outcomes[slot];
    reply(s_guard.requests[slot].request_id, out->state, out->error, out->has_status ? &out->status : NULL);
}

static void save_outcome(size_t slot, const char *state, const char *error, bool status)
{
    s_outcomes[slot] = (command_outcome_t){.state = state, .error = error, .has_status = status};
    if (status) s_outcomes[slot].status = snapshot();
    emit_outcome(slot);
}

static void restore_committed(uint64_t now)
{
    if (esp_base_wifi_apply(&s_context.config.wifi, now) != ESP_OK) s_config_uncertain = true;
}

static void poll_configuration(uint64_t now)
{
    if (!s_trial_active) return;
    const bool ready = s_candidate.wifi.configured ? esp_base_wifi_ready() :
        !strcmp(esp_base_wifi_state(), "unconfigured");
    if (ready && now < s_trial_deadline) {
        esp_base_remote_config_t committed;
        esp_err_t error = esp_base_remote_config_commit_verified(&s_candidate, s_candidate.revision, &committed);
        s_trial_active = false;
        memset(&s_candidate, 0, sizeof s_candidate);
        if (error == ESP_OK) {
            s_context.config = committed;
            save_outcome(s_trial_slot, "succeeded", NULL, true);
        } else if (error == ESP_BASE_CONFIG_UNCERTAIN) {
            s_config_uncertain = true;
            /* A write error may follow a durable commit. Reload before selecting
             * connectivity, never claim the old configuration was restored. */
            if (esp_base_remote_config_load(&committed) == ESP_OK) {
                s_context.config = committed;
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
        memset(&s_candidate, 0, sizeof s_candidate);
        restore_committed(now);
        save_outcome(s_trial_slot, "failed", "connection_proof_failed", false);
    }
}

static void handle_line(const char *line, size_t length, void *context)
{
    (void)context;
    ebase_command_t command;
    const char *error = ebase_parse_command(line, length, &command);
    if (error) { reply(command.request.request_id, "failed", error, NULL); return; }
    if (command.kind == EBASE_STATUS) {
        status_snapshot_t current = snapshot();
        reply(command.request.request_id, "succeeded", NULL, &current);
        return;
    }
    if (command.kind == EBASE_CONFIG_SET) {
        uint8_t bytes[10 + EBASE_CONFIG_BYTES];
        memcpy(bytes, "config.set", 10);
        size_t size = 0;
        if (!ebase_config_encode(&command.config, bytes + 10) ||
            psa_hash_compute(PSA_ALG_SHA_256, bytes, sizeof bytes, command.request.fingerprint,
                sizeof command.request.fingerprint, &size) != PSA_SUCCESS || size != 32) {
            reply(command.request.request_id, "failed", "resource_failure", NULL); return;
        }
    }
    size_t slot = 0;
    const ebase_admission_t decision = ebase_admit(&s_guard, &command.request,
        s_context.device_id, s_boot_id, uptime_ms(), &slot);
    if (decision == EBASE_REPLAY) { emit_outcome(slot); return; }
    if (decision != EBASE_ACCEPT) {
        static const char *const errors[] = {NULL, NULL, "invalid_identity", "wrong_device",
            "wrong_boot", "expired", "invalid_deadline", "request_conflict", "capacity_exceeded"};
        reply(command.request.request_id, decision == EBASE_EXPIRED ? "expired" : "failed", errors[decision], NULL);
        return;
    }
    if (s_trial_active) { save_outcome(slot, "failed", "configuration_busy", false); return; }
    if (s_config_uncertain) { save_outcome(slot, "failed", "storage_uncertain", false); return; }
    if (command.kind == EBASE_CONFIG_SET) {
        if (command.config.revision != s_context.config.revision) {
            save_outcome(slot, "failed", "revision_conflict", false); return;
        }
        if (command.config.revision == UINT32_MAX) { save_outcome(slot, "failed", "revision_exhausted", false); return; }
        s_candidate = command.config;
        s_trial_slot = slot;
        s_trial_deadline = uptime_ms() + 20000;
        save_outcome(slot, "running", NULL, false);
        if (esp_base_wifi_apply(&s_candidate.wifi, uptime_ms()) != ESP_OK) {
            memset(&s_candidate, 0, sizeof s_candidate);
            restore_committed(uptime_ms());
            save_outcome(slot, "failed", "connection_proof_failed", false);
        } else s_trial_active = true;
        return;
    }
    save_outcome(slot, "running", NULL, false);
    (void)fsync(STDOUT_FILENO);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void control_task(void *argument)
{
    (void)argument;
    uint64_t next_report = 0, last_input = 0;
    unsigned char bytes[256];
    while (true) {
        const uint64_t now = uptime_ms();
        esp_base_wifi_poll(now);
        poll_configuration(now);
        if (now >= next_report) { reported(); next_report = now + 5000; }
        if (s_reader.length && now - last_input >= 2000) {
            s_reader.length = 0;
            s_reader.discard = true; /* Never interpret a timed-out tail as a command. */
        }
        size_t count = 0;
        // Official no-driver VFS reads directly from the USB FIFO. Keeping the
        // hardware packet until consumed provides USB backpressure; IDF 6.1's
        // buffered ISR otherwise silently discards bytes on a full RX ring.
        while (count < sizeof bytes && read(STDIN_FILENO, bytes + count, 1) == 1) ++count;
        if (count > 0) {
            last_input = uptime_ms();
            ebase_line_feed(&s_reader, bytes, count, handle_line, NULL);
        }
        vTaskDelay(1); /* Let idle/WDT and other capabilities run under sustained input. */
    }
}

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{
    if (!context || !ebase_is_uuid(context->device_id)) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_base_identity_generate_uuid(s_boot_id, sizeof s_boot_id);
    if (error != ESP_OK) return error;
    usb_serial_jtag_vfs_use_nonblocking();
    // In the official no-driver VFS, reads are always nonblocking. The descriptor
    // flag must remain clear so IDF 6.1 prefetches from hardware (there is no ring).
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0) return ESP_FAIL;
    s_context = *context;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    error = esp_base_wifi_start(&s_context.config.wifi);
    if (error != ESP_OK) return error;
    if (xTaskCreate(control_task, "base_control", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
