// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_base_config.h"
#include "esp_err.h"

typedef struct {
    const char *state;
    int32_t error;
    uint64_t attempts;
    uint64_t ready_sessions;
    uint64_t retries;
    uint64_t pongs;
    uint64_t work_completed;
    uint64_t work_failed;
    uint32_t work_active;
    uint32_t work_waiting;
} esp_base_frp_snapshot_t;

/* The USB control task is the sole caller. A new revision first destroys the
 * previous library instance; ESP_ERR_TIMEOUT means cleanup is still active and
 * the caller must retry without dropping the old handle. */
esp_err_t esp_base_frp_owner_configure(const ebase_frp_config_t *config,
                                       const char *device_id);
/* The endpoint gate is true only after the separate, authenticated loopback
 * management listener is bound to config.local_port. A failed bind keeps
 * FRPS disconnected rather than exposing an unauthenticated local target. */
void esp_base_frp_owner_poll(uint64_t now_ms, bool network_ready,
                             bool trusted_time_ready, bool endpoint_ready);
esp_base_frp_snapshot_t esp_base_frp_owner_snapshot(void);
