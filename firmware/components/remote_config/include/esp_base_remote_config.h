// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_config.h"
#include "esp_err.h"

#define ESP_BASE_CONFIG_CONFLICT 0x7401
#define ESP_BASE_CONFIG_EXHAUSTED 0x7402
#define ESP_BASE_CONFIG_UNCERTAIN 0x7403

/* Single control owner only. No erase or migration fallback on any error. */
esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config);
/* Call only AFTER candidate connection proof. Persists revision + payload as
 * one NVS blob, then reads it back. Any error after set_blob was attempted is
 * UNCERTAIN: never assume rollback or retry; reload the durable state. */
esp_err_t esp_base_remote_config_commit_verified(const esp_base_remote_config_t *candidate,
                                                uint32_t expected_revision,
                                                esp_base_remote_config_t *committed);
