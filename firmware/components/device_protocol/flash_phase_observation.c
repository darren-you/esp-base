// SPDX-License-Identifier: Apache-2.0
#include "esp_base_flash_observation.h"
#include "esp_base_flash_phase.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PHASE_COUNT 16U
_Static_assert(sizeof(emqtt_capacity_context_t) == 4U * sizeof(uint32_t),
    "Activity identity must contain exactly four integers");

typedef struct {
    uint32_t begun, completed, sample_count;
    emqtt_capacity_context_t identity;
    emqtt_capacity_sample_t sample;
    uint64_t call_started_us, call_finished_us;
} phase_record_t;
static portMUX_TYPE s_phase_lock = portMUX_INITIALIZER_UNLOCKED;
static phase_record_t s_phases[PHASE_COUNT];
static bool s_dirty[PHASE_COUNT];
static uint32_t s_report_remaining;
static esp_base_flash_scope_t s_scope;
static uint32_t s_context_id, s_call_id;
static bool s_bound, s_valid = true;
static emqtt_capacity_context_t s_current;
/* Retain the last call until the next begin, for the callback delivered just
 * after its end. A later call/context makes that old delivery ineligible. */
static emqtt_capacity_context_t s_last;
static uint64_t s_call_started_us, s_call_finished_us;
static const char *const s_names[PHASE_COUNT] = {
    "inactive_erase", "ota_begin", "app_write_may_erase", "source_or_recovery_image_verify",
    "ota_end_verify", "boot_select_or_restore_api", "boot_confirm_api", "boot_invalidate_api",
    "intent_set_blob", "intent_commit", "failure_set_blob", "failure_commit",
    "success_set_blob", "success_commit", "prepare_image_verify", "select_image_verify",
};

static bool uuid(const char *value)
{
    if (!value || strnlen(value, 37) != 36 || value[14] != '4' ||
        !strchr("89ab", value[19])) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!((value[i] >= '0' && value[i] <= '9') ||
                   (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

void esp_base_flash_phase_bind(const char *boot_id, const char *operation_id,
    const char *request_id, bool recovery)
{
    const bool identity_valid = uuid(boot_id) && uuid(operation_id) &&
        (recovery ? request_id == NULL : uuid(request_id));
    portENTER_CRITICAL(&s_phase_lock);
    if (!identity_valid || s_bound || s_current.valid || s_context_id == UINT32_MAX) s_valid = false;
    if (s_valid) {
        ++s_context_id;
        s_bound = true;
        memset(s_phases, 0, sizeof s_phases);
        memset(s_dirty, 0, sizeof s_dirty);
        s_report_remaining = 0;
        s_scope = ESP_BASE_FLASH_SCOPE_OTHER;
        s_last = (emqtt_capacity_context_t){0};
    }
    const uint32_t context_id = s_context_id;
    const bool valid = s_valid;
    portEXIT_CRITICAL(&s_phase_lock);
    /* No caller Flash/MQTT lock may be held here. Identity strings never enter
     * the producer's short critical section or MQTT callback. */
    if (identity_valid)
        printf("ESP_BASE_FLASH_CONTEXT boot_id=%s context_id=%" PRIu32 " operation_id=%s request_id=%s origin=%s valid=%u\n",
            boot_id, context_id, operation_id, recovery ? "none" : request_id,
            recovery ? "recovery" : "request", (unsigned)valid);
}

void esp_base_flash_phase_unbind(void)
{
    portENTER_CRITICAL(&s_phase_lock);
    if (s_current.valid) s_valid = false;
    s_bound = false;
    s_report_remaining = 0;
    s_scope = ESP_BASE_FLASH_SCOPE_OTHER;
    s_current = s_last = (emqtt_capacity_context_t){0};
    portEXIT_CRITICAL(&s_phase_lock);
}

void esp_base_flash_phase_scope(esp_base_flash_scope_t scope)
{
    portENTER_CRITICAL(&s_phase_lock);
    if (scope < ESP_BASE_FLASH_SCOPE_OTHER || scope > ESP_BASE_FLASH_SCOPE_SELECT || s_current.valid)
        s_valid = false;
    else s_scope = scope;
    if (!s_valid) {
        s_current = (emqtt_capacity_context_t){0};
        memset(s_dirty, 1, sizeof s_dirty);
    }
    portEXIT_CRITICAL(&s_phase_lock);
}

void esp_base_flash_phase_observe(void *context, uint32_t stage, bool active)
{
    (void)context;
    const int64_t started = active ? esp_timer_get_time() : 0;
    uint32_t finishing_call = 0;
    portENTER_CRITICAL(&s_phase_lock);
    if (s_bound && s_valid) {
        const bool supported_stage = stage >= 1U && stage <= 14U;
        if (stage == 4U && s_scope == ESP_BASE_FLASH_SCOPE_PREPARE) stage = 15U;
        else if (stage == 4U && s_scope == ESP_BASE_FLASH_SCOPE_SELECT) stage = 16U;
        if (stage > 0 && stage <= PHASE_COUNT) s_dirty[stage - 1U] = true;
        if (!supported_stage || started < 0) s_valid = false;
        else if (active) {
            phase_record_t *record = &s_phases[stage - 1U];
            if (s_current.valid || s_call_id == UINT32_MAX || record->begun == UINT32_MAX ||
                (uint64_t)started < s_call_started_us || (uint64_t)started < s_call_finished_us) s_valid = false;
            else {
                ++record->begun;
                s_current = (emqtt_capacity_context_t){1U, s_context_id, stage, ++s_call_id};
                s_last = s_current;
                s_call_started_us = (uint64_t)started;
                s_call_finished_us = 0;
            }
        } else {
            phase_record_t *record = &s_phases[stage - 1U];
            if (!s_current.valid || s_current.phase_id != stage || record->completed == UINT32_MAX) s_valid = false;
            else {
                ++record->completed;
                finishing_call = s_current.call_id;
                s_current = (emqtt_capacity_context_t){0};
            }
        }
    }
    if (!s_valid) {
        s_current = (emqtt_capacity_context_t){0};
        memset(s_dirty, 1, sizeof s_dirty);
    }
    portEXIT_CRITICAL(&s_phase_lock);
    if (!finishing_call) return;
    /* Close the activity before reading its upper endpoint. A sample already
     * admitted by MQTT can arrive in this gap; no new stable active sample can
     * begin. Clocks stay outside the short lock. Until the endpoint is copied,
     * a periodic row is conservatively unfinished. */
    const int64_t finished = esp_timer_get_time();
    portENTER_CRITICAL(&s_phase_lock);
    if (!s_valid || !s_bound || s_last.call_id != finishing_call || s_current.valid ||
        finished < 0 || (uint64_t)finished < s_call_started_us) {
        s_valid = false;
        s_current = (emqtt_capacity_context_t){0};
        memset(s_dirty, 1, sizeof s_dirty);
    } else {
        s_call_finished_us = (uint64_t)finished;
        phase_record_t *record = &s_phases[stage - 1U];
        if (record->identity.call_id == finishing_call)
            record->call_finished_us = s_call_finished_us;
        s_dirty[stage - 1U] = true;
    }
    portEXIT_CRITICAL(&s_phase_lock);
}

void esp_base_flash_phase_read(void *context, emqtt_capacity_context_t *out)
{
    (void)context;
    if (!out) return;
    portENTER_CRITICAL(&s_phase_lock);
    *out = s_valid && s_bound ? s_current : (emqtt_capacity_context_t){0};
    portEXIT_CRITICAL(&s_phase_lock);
}

void esp_base_flash_phase_sample(void *context,
    const emqtt_capacity_context_t *identity, const emqtt_capacity_sample_t *sample)
{
    (void)context;
    if (!identity || !sample) return;
    portENTER_CRITICAL(&s_phase_lock);
    if (s_valid && s_bound && identity->valid == 1U && identity->phase_id > 0 &&
        identity->phase_id <= PHASE_COUNT &&
        memcmp(identity, &s_last, sizeof *identity) == 0 &&
        sample->started_us >= s_call_started_us && sample->finished_us >= sample->started_us &&
        (!s_call_finished_us || sample->finished_us <= s_call_finished_us)) {
        phase_record_t *record = &s_phases[identity->phase_id - 1U];
        s_dirty[identity->phase_id - 1U] = true;
        if (record->sample_count == UINT32_MAX) s_valid = false;
        else {
            ++record->sample_count;
            /* Keep a whole observed tuple. Never combine owner/outbox maxima. */
            if (!record->identity.valid || sample->owned_request_bytes > record->sample.owned_request_bytes ||
                (sample->owned_request_bytes == record->sample.owned_request_bytes &&
                 sample->outbox_wire_bytes >= record->sample.outbox_wire_bytes)) {
                record->identity = *identity;
                record->sample = *sample;
                record->call_started_us = s_call_started_us;
                record->call_finished_us = s_call_finished_us;
            }
        }
    }
    portEXIT_CRITICAL(&s_phase_lock);
}

static void emit_phase_row(const char *boot_id, unsigned i)
{
    phase_record_t row;
    const int64_t started = esp_timer_get_time();
    portENTER_CRITICAL(&s_phase_lock);
    const bool publish = s_bound && s_dirty[i];
    row = s_phases[i];
    s_dirty[i] = false;
    const bool valid = s_valid;
    const uint32_t context_id = s_context_id;
    portEXIT_CRITICAL(&s_phase_lock);
    const int64_t finished = esp_timer_get_time();
    if (!publish || !row.begun) return;
    printf("ESP_BASE_FLASH_JOINT boot_id=%s context_id=%" PRIu32 " phase_id=%u phase=%s snapshot_started_us=%" PRId64 " snapshot_finished_us=%" PRId64 " valid=%u begun=%" PRIu32 " completed=%" PRIu32 " sample_count=%" PRIu32 " call_id=%" PRIu32 " call_started_us=%" PRIu64 " call_finished_us=%" PRIu64 " sample_started_us=%" PRIu64 " sample_finished_us=%" PRIu64 " runtime_instance=%" PRIu32 " complete_owner_count=%" PRIu32 " complete_payload_bytes=%" PRIu32 " partial_declared_bytes=%" PRIu32 " partial_received_bytes=%" PRIu32 " owned_request_bytes=%" PRIu32 " outbox_wire_bytes=%" PRIu32 " owned_message_metadata_bytes=%" PRIu32 " observation_storage_bytes=%u snapshot_copy_bytes=%u\n",
        boot_id, context_id, i + 1U, s_names[i], started, finished, (unsigned)valid,
        row.begun, row.completed, row.sample_count, row.identity.call_id,
        row.call_started_us, row.call_finished_us, row.sample.started_us, row.sample.finished_us,
        row.sample.runtime_instance, row.sample.complete_owner_count, row.sample.complete_payload_bytes,
        row.sample.partial_declared_bytes, row.sample.partial_received_bytes, row.sample.owned_request_bytes,
        row.sample.outbox_wire_bytes, row.sample.owned_message_metadata_bytes,
        (unsigned)(sizeof s_phases + sizeof s_dirty + sizeof s_report_remaining + sizeof s_scope + sizeof s_phase_lock + sizeof s_context_id + sizeof s_call_id +
            sizeof s_bound + sizeof s_valid + sizeof s_current + sizeof s_last +
            sizeof s_call_started_us + sizeof s_call_finished_us), (unsigned)sizeof row);
}

void esp_base_flash_phase_report_request(void)
{
    portENTER_CRITICAL(&s_phase_lock);
    if (s_bound)
        for (unsigned i = 0; i < PHASE_COUNT; ++i)
            if (s_dirty[i]) s_report_remaining |= UINT32_C(1) << i;
    portEXIT_CRITICAL(&s_phase_lock);
}

void esp_base_flash_phase_poll(const char *boot_id)
{
    if (!uuid(boot_id)) return;
    unsigned selected = PHASE_COUNT;
    portENTER_CRITICAL(&s_phase_lock);
    for (unsigned i = 0; i < PHASE_COUNT; ++i) {
        if (s_report_remaining & (UINT32_C(1) << i)) {
            selected = i;
            s_report_remaining &= ~(UINT32_C(1) << i);
            break;
        }
    }
    portEXIT_CRITICAL(&s_phase_lock);
    if (selected < PHASE_COUNT) emit_phase_row(boot_id, selected);
}

/* Terminal/reboot only: call after physical Flash/MQTT locks are released. */
void esp_base_flash_phase_emit(const char *boot_id)
{
    if (!uuid(boot_id)) return;
    portENTER_CRITICAL(&s_phase_lock);
    s_report_remaining = 0;
    portEXIT_CRITICAL(&s_phase_lock);
    for (unsigned i = 0; i < PHASE_COUNT; ++i) emit_phase_row(boot_id, i);
}
