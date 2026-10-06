// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_base_config.h"

typedef enum {
    ESP_BASE_FRP_MANAGEMENT_STATUS,
    ESP_BASE_FRP_MANAGEMENT_RESTART,
    ESP_BASE_FRP_MANAGEMENT_FIRMWARE_STATUS,
    ESP_BASE_FRP_MANAGEMENT_OTA_START,
    ESP_BASE_FRP_MANAGEMENT_OTA_RESULT,
    ESP_BASE_FRP_MANAGEMENT_BUSINESS_STATUS,
    ESP_BASE_FRP_MANAGEMENT_BUSINESS_PAUSE,
    ESP_BASE_FRP_MANAGEMENT_BUSINESS_RESUME,
} esp_base_frp_management_command_t;

typedef int (*esp_base_frp_management_handler_t)(esp_base_frp_management_command_t command,
                                              const uint8_t *request, size_t length,
                                              char *response, size_t capacity,
                                              size_t *response_length, void *context);

/* Called only by the control owner. Reconfiguration closes the old listener
 * and any partial request before replacing the independent FRP key. */
void esp_base_frp_management_listener_configure(const ebase_frp_config_t *config);
void esp_base_frp_management_listener_poll(uint64_t now_ms,
                                       esp_base_frp_management_handler_t handler,
                                       void *context);
bool esp_base_frp_management_listener_ready(void);
/* Owner-only: a response has been prepared but has not finished sending. */
bool esp_base_frp_management_listener_response_pending(void);

/* One caller-owned firmware operation may arm one authenticated upload after
 * its intent has been persisted and read back. Inputs are copied; an existing
 * arm/upload refuses a second operation. Headers and authentication must arrive
 * within 5000 ms of now_ms. The bound key survives only until finish. */
bool esp_base_frp_management_upload_arm(const char *operation_id, const char *device_id,
    const char *boot_id, uint32_t image_size_bytes, const uint8_t sha256[32], uint64_t now_ms);
bool esp_base_frp_management_upload_connected(void);
/* eota_stream_t.read-compatible: at most timeout_ms of socket waiting, zero
 * only at the authenticated Content-Length boundary, -2 for retryable timeout,
 * other negative results for disconnect/cancel/extra bytes. No Flash ownership
 * is held by this mechanism. The worker exclusively reads/finishes the stream. */
int esp_base_frp_management_upload_read(void *context, uint8_t *buffer, size_t capacity,
    uint32_t timeout_ms);
/* Cancel wakes a connected reader with shutdown, while the worker retains
 * ownership of close. finish always releases the arm and fd, including an arm
 * with no connection; responses use the arm's key, never a reconfigured key.
 * This reports only receive/prepare state, not post-reboot operation success. */
void esp_base_frp_management_upload_cancel(void);
bool esp_base_frp_management_upload_finish(int status, const char *body,
    size_t body_length, uint32_t timeout_ms);
