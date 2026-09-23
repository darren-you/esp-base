// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_owner.h"
#include "esp_base_time.h"
#include "esp_frp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct efrp_client { unsigned marker; };
static struct efrp_client client_object;
static unsigned creates, starts, destroys, blocked_destroys;
static efrp_phase_t phase = EFRP_PHASE_READY;
static bool time_ready = true;

bool esp_base_time_ready(void) { return time_ready; }

efrp_result_t efrp_create(const efrp_config_t *c, efrp_client_t **out)
{
    assert(c && out && !*out);
    assert(!strcmp(c->server_hostname, "frp.example.test") && c->server_port == 7000);
    assert(c->ca_length == strlen("-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n"));
    assert(c->token_length == strlen("test-frp-token") &&
           !memcmp(c->token, "test-frp-token", c->token_length));
    assert(!strcmp(c->client_id, "22222222-2222-4222-8222-222222222222"));
    assert(!strcmp(c->proxy_name, "base-device") && c->remote_port == 10200);
    assert(c->local_ipv4[0] == 127 && c->local_ipv4[1] == 0 &&
           c->local_ipv4[2] == 0 && c->local_ipv4[3] == 1 && c->local_port == 8123);
    assert(c->time_is_trusted(c->context) == time_ready);
    *out = &client_object; ++creates; return EFRP_OK;
}
efrp_result_t efrp_start(efrp_client_t *c)
{
    assert(c == &client_object); ++starts; return EFRP_OK;
}
efrp_result_t efrp_destroy(efrp_client_t **c, uint32_t timeout)
{
    assert(c && *c == &client_object && timeout == 0); ++destroys;
    if (blocked_destroys) { --blocked_destroys; return EFRP_WOULD_BLOCK; }
    *c = NULL; return EFRP_OK;
}
efrp_result_t efrp_get_status(efrp_client_t *c, efrp_status_t *status)
{
    assert(c == &client_object && status);
    *status = (efrp_status_t){.phase = phase, .attempts = 2, .ready_sessions = 1,
                              .pongs = 3, .work.completed = 4};
    return EFRP_OK;
}

int main(void)
{
    const char *device = "22222222-2222-4222-8222-222222222222";
    ebase_frp_config_t config = {.configured = true, .server_port = 7000,
                                .remote_port = 10200, .local_port = 8123};
    strcpy(config.server_hostname, "frp.example.test");
    strcpy(config.token, "test-frp-token");
    strcpy(config.ca_pem, "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n");
    strcpy(config.proxy_name, "base-device");
    config.management_key[0] = 2;
    assert(esp_base_frp_owner_configure(NULL, device) == ESP_ERR_INVALID_ARG);
    assert(esp_base_frp_owner_configure(&config, device) == ESP_OK);
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "endpoint_unavailable"));
    esp_base_frp_owner_poll(100, true, true, false);
    assert(!creates && !starts);
    esp_base_frp_owner_poll(200, true, true, true);
    assert(creates == 1 && starts == 1);
    const esp_base_frp_snapshot_t ready = esp_base_frp_owner_snapshot();
    assert(!strcmp(ready.state, "ready") && ready.attempts == 2 &&
           ready.ready_sessions == 1 && ready.pongs == 3 && ready.work_completed == 4);
    blocked_destroys = 1;
    esp_base_frp_owner_poll(300, false, true, true);
    assert(destroys == 1 && creates == 1);
    esp_base_frp_owner_poll(400, true, true, true);
    assert(destroys == 2 && creates == 2 && starts == 2);
    phase = EFRP_PHASE_BACKOFF;
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "backoff"));
    blocked_destroys = 2;
    assert(esp_base_frp_owner_configure(&config, device) == ESP_ERR_TIMEOUT);
    esp_base_frp_owner_poll(450, true, true, true);
    assert(creates == 2 && starts == 2);
    assert(esp_base_frp_owner_configure(&config, device) == ESP_OK);
    esp_base_frp_owner_poll(500, true, false, true);
    assert(creates == 2);
    time_ready = false;
    esp_base_frp_owner_poll(600, true, true, true);
    assert(creates == 2);
    time_ready = true;
    esp_base_frp_owner_poll(700, true, true, true);
    assert(creates == 3 && starts == 3);
    config = (ebase_frp_config_t){0};
    assert(esp_base_frp_owner_configure(&config, device) == ESP_OK);
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "unconfigured"));
    assert(destroys == 6);
    puts("  frp_owner       passed (endpoint gate, status, stop/reconfigure convergence)");
}
