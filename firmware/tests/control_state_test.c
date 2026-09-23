// SPDX-License-Identifier: Apache-2.0
#include "control_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int64_t now_us;

int64_t esp_timer_get_time(void)
{
    return now_us;
}

int main(void)
{
    esp_base_control_state_t state = {0};
    now_us = 1000 * 1000;
    assert(!esp_base_control_state_is_recent(&state));
    assert(esp_base_control_state_progress_count(&state) == 0);
    assert(esp_base_control_state_config_write_error(&state) == NULL);
    esp_base_control_state_set_ota_pending(&state, true);
    assert(strcmp(esp_base_control_state_config_write_error(&state), "ota_verification_pending") == 0);
    esp_base_control_state_note_progress(&state);
    assert(esp_base_control_state_is_recent(&state));
    assert(esp_base_control_state_progress_count(&state) == 1);

    now_us = (1000 + ESP_BASE_CONTROL_MAX_STALE_MS) * INT64_C(1000);
    assert(esp_base_control_state_is_recent(&state));
    ++now_us;
    assert(esp_base_control_state_is_recent(&state));
    now_us += 1000;
    assert(!esp_base_control_state_is_recent(&state));

    /* A legal multi-second control iteration can finish and refresh progress. */
    esp_base_control_state_note_progress(&state);
    assert(esp_base_control_state_progress_count(&state) == 2);
    now_us += 4000 * INT64_C(1000);
    assert(esp_base_control_state_is_recent(&state));

    /* Millisecond counter wrap does not turn recent progress into a failure. */
    now_us = (INT64_C(1) << 32) * 1000 - 1000 * 1000;
    esp_base_control_state_note_progress(&state);
    assert(esp_base_control_state_progress_count(&state) == 3);
    now_us += 3000 * INT64_C(1000);
    assert(esp_base_control_state_is_recent(&state));
    now_us += 3000 * INT64_C(1000);
    assert(!esp_base_control_state_is_recent(&state));

    esp_base_control_state_set_ota_pending(&state, false);
    assert(esp_base_control_state_config_write_error(&state) == NULL);
    esp_base_control_state_set_ota_download_active(&state, true);
    assert(strcmp(esp_base_control_state_config_write_error(&state), "ota_in_progress") == 0);
    esp_base_control_state_set_ota_pending(&state, true);
    assert(esp_base_control_state_ota_pending(&state));
    assert(strcmp(esp_base_control_state_config_write_error(&state), "ota_verification_pending") == 0);
    esp_base_control_state_set_ota_pending(&state, false);
    esp_base_control_state_set_ota_download_active(&state, false);
    assert(!esp_base_control_state_ota_pending(&state));
    assert(esp_base_control_state_config_write_error(&state) == NULL);

    puts("  control_state passed (progress, 5 s boundary, pending/download write gates, wrap)");
}
