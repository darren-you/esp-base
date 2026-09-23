#include "control_state.h"

#include <stddef.h>

#include "esp_timer.h"

void esp_base_control_state_note_progress(esp_base_control_state_t *state)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    atomic_store_explicit(&state->last_progress_ms, now_ms, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->progress_count, 1, memory_order_release);
    atomic_store_explicit(&state->observed, true, memory_order_release);
}

uint32_t esp_base_control_state_progress_count(const esp_base_control_state_t *state)
{
    return (uint32_t)atomic_load_explicit(&state->progress_count, memory_order_acquire);
}

bool esp_base_control_state_is_recent(const esp_base_control_state_t *state)
{
    if (!atomic_load_explicit(&state->observed, memory_order_acquire)) {
        return false;
    }
    const uint32_t last_ms = (uint32_t)atomic_load_explicit(&state->last_progress_ms, memory_order_relaxed);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return (uint32_t)(now_ms - last_ms) <= ESP_BASE_CONTROL_MAX_STALE_MS;
}

void esp_base_control_state_set_ota_pending(esp_base_control_state_t *state, bool pending)
{
    atomic_store_explicit(&state->ota_verification_pending, pending, memory_order_release);
}

bool esp_base_control_state_ota_pending(const esp_base_control_state_t *state)
{
    return atomic_load_explicit(&state->ota_verification_pending, memory_order_acquire);
}

void esp_base_control_state_set_ota_download_active(esp_base_control_state_t *state, bool active)
{
    atomic_store_explicit(&state->ota_download_active, active, memory_order_release);
}

const char *esp_base_control_state_config_write_error(const esp_base_control_state_t *state)
{
    if (atomic_load_explicit(&state->ota_verification_pending, memory_order_acquire)) {
        return "ota_verification_pending";
    }
    return atomic_load_explicit(&state->ota_download_active, memory_order_acquire) ?
           "ota_in_progress" : NULL;
}
