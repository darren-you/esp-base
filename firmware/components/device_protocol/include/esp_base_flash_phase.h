// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "emqtt.h"

/* MQTT SDK callback bridge. No caller of these short-copy functions may hold
 * a lock whose other holders call MQTT; no output/allocation occurs here. */
void esp_base_flash_phase_read(void *context, emqtt_capacity_context_t *out);
void esp_base_flash_phase_sample(void *context, const emqtt_capacity_context_t *identity,
    const emqtt_capacity_sample_t *sample);
