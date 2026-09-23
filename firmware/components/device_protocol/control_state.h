#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define ESP_BASE_CONTROL_MAX_STALE_MS UINT32_C(5000)

typedef struct {
    atomic_uint_fast32_t last_progress_ms;
    atomic_uint_fast32_t progress_count;
    atomic_bool observed;
    atomic_bool ota_verification_pending;
    atomic_bool ota_download_active;
} esp_base_control_state_t;

void esp_base_control_state_note_progress(esp_base_control_state_t *state);
bool esp_base_control_state_is_recent(const esp_base_control_state_t *state);
uint32_t esp_base_control_state_progress_count(const esp_base_control_state_t *state);
void esp_base_control_state_set_ota_pending(esp_base_control_state_t *state, bool pending);
bool esp_base_control_state_ota_pending(const esp_base_control_state_t *state);
void esp_base_control_state_set_ota_download_active(esp_base_control_state_t *state, bool active);
const char *esp_base_control_state_config_write_error(const esp_base_control_state_t *state);
