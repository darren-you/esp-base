// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_owner.h"
#include "esp_base_time.h"
#include "esp_frp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct efrp_client { unsigned marker; };
static struct efrp_client client_object;
static efrp_aead_flash_store_t scratch_store;
static unsigned creates, starts, destroys, blocked_destroys, storage_destroys;
static efrp_phase_t phase = EFRP_PHASE_READY;
static bool time_ready = true;
static const ebase_frp_config_t *expected_config;

bool esp_base_time_ready(void) { return time_ready; }

efrp_result_t efrp_create(const efrp_config_t *c, efrp_client_t **out)
{
    assert(c && out && !*out);
    assert(c->run_id && !strcmp(c->run_id, c->client_id));
    assert(expected_config != NULL && expected_config->configured);
    assert(c->server_hostname == expected_config->server_hostname &&
           c->ca_pem == (const uint8_t *)expected_config->ca_pem &&
           c->token == (const uint8_t *)expected_config->token &&
           c->proxy_name == expected_config->proxy_name);
    assert(!strcmp(c->server_hostname, expected_config->server_hostname) &&
           c->server_port == expected_config->server_port);
    assert(c->ca_length == strlen("-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n"));
    assert(c->token_length == strlen("test-frp-token") &&
           !memcmp(c->token, "test-frp-token", c->token_length));
    assert(!strcmp(c->client_id, "22222222-2222-4222-8222-222222222222"));
    assert(!strcmp(c->proxy_name, "base-device") && c->remote_port == 10200);
    assert(c->local_ipv4[0] == 127 && c->local_ipv4[1] == 0 &&
           c->local_ipv4[2] == 0 && c->local_ipv4[3] == 1 && c->local_port == 8123);
    assert(c->time_is_trusted(c->context) == time_ready);
    assert(c->flash_store == &scratch_store);
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
    if (storage_destroys) { --storage_destroys; return EFRP_STORAGE_ERROR; }
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
    expected_config = &config;
    assert(esp_base_frp_owner_configure(NULL, device, &scratch_store) == ESP_ERR_INVALID_ARG);
    assert(esp_base_frp_owner_configure(&config, device, NULL) == ESP_ERR_INVALID_STATE);
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "failed"));
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_OK);
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
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_ERR_TIMEOUT);
    esp_base_frp_owner_poll(450, true, true, true);
    assert(creates == 2 && starts == 2);
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_OK);
    esp_base_frp_owner_poll(500, true, false, true);
    assert(creates == 2);
    time_ready = false;
    esp_base_frp_owner_poll(600, true, true, true);
    assert(creates == 2);
    time_ready = true;
    esp_base_frp_owner_poll(700, true, true, true);
    assert(creates == 3 && starts == 3);
    storage_destroys = 1;
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_ERR_TIMEOUT);
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "stopping"));
    esp_base_frp_owner_poll(710, true, true, true);
    assert(creates == 3 && starts == 3); /* old handle retained until clear retry */
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_OK);
    esp_base_frp_owner_poll(720, true, true, true);
    assert(creates == 4 && starts == 4);
    config = (ebase_frp_config_t){0};
    assert(esp_base_frp_owner_configure(&config, device, NULL) == ESP_OK);
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "unconfigured"));
    assert(destroys == 8);
    /* In-place canonical replacement must not start a client while the old
     * native worker/Flash store still has an unresolved destroy. */
    config = (ebase_frp_config_t){.configured = true, .server_port = 7000,
                                .remote_port = 10200, .local_port = 8123};
    strcpy(config.server_hostname, "frp.example.test");
    strcpy(config.token, "test-frp-token");
    strcpy(config.ca_pem, "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n");
    strcpy(config.proxy_name, "base-device");
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_OK);
    esp_base_frp_owner_poll(800, true, true, true);
    assert(creates == 5U);
    strcpy(config.server_hostname, "replacement-frp.example.test");
    config.server_port = 7443;
    storage_destroys = 1U;
    blocked_destroys = 1U;
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_ERR_TIMEOUT);
    assert(!strcmp(esp_base_frp_owner_snapshot().state, "stopping"));
    esp_base_frp_owner_poll(810, true, true, true);
    assert(creates == 5U && !strcmp(esp_base_frp_owner_snapshot().state, "stopping"));
    assert(esp_base_frp_owner_configure(&config, device, &scratch_store) == ESP_OK);
    esp_base_frp_owner_poll(820, true, true, true);
    assert(creates == 6U);
    /* Reconnect reads the admitted canonical address, after old destroy. */
    esp_base_frp_owner_poll(830, false, true, true);
    esp_base_frp_owner_poll(840, true, true, true);
    assert(creates == 7U);
    /* Disabled canonical data may overwrite the old source before cleanup.
     * Pending cleanup never dereferences it or creates another native client. */
    config = (ebase_frp_config_t){0};
    storage_destroys = 1U;
    blocked_destroys = 1U;
    assert(esp_base_frp_owner_configure(&config, device, NULL) == ESP_ERR_TIMEOUT);
    esp_base_frp_owner_poll(850, true, true, true);
    assert(creates == 7U && !strcmp(esp_base_frp_owner_snapshot().state, "stopping"));
    assert(esp_base_frp_owner_configure(&config, device, NULL) == ESP_OK);
    esp_base_frp_owner_poll(860, true, true, true);
    assert(creates == 7U && !strcmp(esp_base_frp_owner_snapshot().state, "unconfigured"));
    puts("  frp_owner       passed (canonical borrow, drain, reconnect, in-place revision and disable)");
}
