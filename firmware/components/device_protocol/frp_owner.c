// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_owner.h"
#include "esp_base_time.h"
#include "esp_frp.h"
#include <string.h>

static ebase_frp_config_t s_config;
static efrp_client_t *s_client;
static char s_device_id[37];
static uint64_t s_retry_at_ms;
static int32_t s_error;
static bool s_endpoint_ready, s_network_ready, s_draining, s_reconfiguring;

static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
}

static bool trusted_time(void *context)
{
    (void)context;
    return esp_base_time_ready();
}

static bool release_client(void)
{
    if (!s_client) { s_draining = false; return true; }
    const efrp_result_t result = efrp_destroy(&s_client, 0);
    if (result == EFRP_OK) { s_draining = false; return true; }
    s_draining = true;
    if (result != EFRP_WOULD_BLOCK && result != EFRP_TIMEOUT) s_error = result;
    return false;
}

esp_err_t esp_base_frp_owner_configure(const ebase_frp_config_t *config,
                                       const char *device_id)
{
    if (!config || !device_id || strlen(device_id) != 36) return ESP_ERR_INVALID_ARG;
    /* Keep the old config and handle until destroy proves worker, DNS and all
     * callbacks have finished. The caller retries this revision each pass. */
    s_reconfiguring = true;
    if (!release_client()) return ESP_ERR_TIMEOUT;
    wipe(&s_config, sizeof s_config);
    s_config = *config;
    memcpy(s_device_id, device_id, sizeof s_device_id);
    s_retry_at_ms = 0;
    s_error = 0;
    s_endpoint_ready = s_network_ready = false;
    s_reconfiguring = false;
    return ESP_OK;
}

void esp_base_frp_owner_poll(uint64_t now_ms, bool network_ready,
                             bool trusted_time_ready, bool endpoint_ready)
{
    s_network_ready = network_ready && trusted_time_ready && esp_base_time_ready();
    s_endpoint_ready = endpoint_ready;
    if (s_reconfiguring || !s_config.configured || !s_network_ready || !s_endpoint_ready) {
        (void)release_client();
        return;
    }
    if (s_draining && !release_client()) return;
    if (s_client || now_ms < s_retry_at_ms) return;
    const efrp_config_t config = {
        .server_hostname = s_config.server_hostname,
        .server_port = s_config.server_port,
        .ca_pem = (const uint8_t *)s_config.ca_pem,
        .ca_length = strlen(s_config.ca_pem),
        .token = (const uint8_t *)s_config.token,
        .token_length = strlen(s_config.token),
        .client_id = s_device_id,
        .proxy_name = s_config.proxy_name,
        .remote_port = s_config.remote_port,
        .local_ipv4 = {127, 0, 0, 1},
        .local_port = s_config.local_port,
        .time_is_trusted = trusted_time,
    };
    efrp_result_t result = efrp_create(&config, &s_client);
    if (result == EFRP_OK) result = efrp_start(s_client);
    if (result != EFRP_OK) {
        s_error = result;
        s_retry_at_ms = now_ms + 5000;
        (void)release_client();
    }
}

esp_base_frp_snapshot_t esp_base_frp_owner_snapshot(void)
{
    esp_base_frp_snapshot_t out = {0};
    out.error = s_error;
    if (!s_config.configured) out.state = "unconfigured";
    else if (!s_endpoint_ready) out.state = "endpoint_unavailable";
    else if (!s_network_ready) out.state = "network_unavailable";
    else if (s_draining || s_reconfiguring) out.state = "stopping";
    else if (s_client) {
        efrp_status_t status = {0};
        const efrp_result_t result = efrp_get_status(s_client, &status);
        if (result == EFRP_OK) {
            out.error = status.error;
            out.attempts = status.attempts;
            out.ready_sessions = status.ready_sessions;
            out.retries = status.retries;
            out.pongs = status.pongs;
            out.work_completed = status.work.completed;
            out.work_failed = status.work.failed;
            out.work_active = status.work.active;
            out.work_waiting = status.work.waiting;
            out.state = status.phase == EFRP_PHASE_READY ? "ready" :
                status.phase == EFRP_PHASE_BACKOFF ? "backoff" :
                status.phase == EFRP_PHASE_FAILED ? "failed" :
                status.phase == EFRP_PHASE_DRAINING || status.phase == EFRP_PHASE_STOPPED ? "stopping" : "connecting";
        } else {
            out.state = "unknown";
            out.error = result;
        }
    } else out.state = s_error ? "failed" : "connecting";
    return out;
}
