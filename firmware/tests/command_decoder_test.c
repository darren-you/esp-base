// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command.h"
#include "esp_base_ota_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST "11111111-1111-4111-8111-111111111111"
#define DEVICE "22222222-2222-4222-8222-222222222222"
#define BOOT "33333333-3333-4333-8333-333333333333"
static const char status[] = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"status\"}";
static const char restart[] = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}";
static void reject(const char *json)
{
    ebase_command_t out = {0};
    assert(ebase_parse_command(json, strlen(json), &out, malloc) != NULL);
    ebase_command_release(&out);
}
static unsigned lines, invalid;
static void receive(const char *line, size_t length, void *context)
{
    (void)context;
    ++lines;
    if (!line) { ++invalid; return; }
    ebase_command_t out = {0};
    assert(!ebase_parse_command(line, length, &out, malloc));
    assert(out.kind == EBASE_STATUS);
    ebase_command_release(&out);
}
static void config_tests(void)
{
    char json[2048];
    const char *prefix = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"config.set\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":";
    const char *valid[] = {
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":{\"ssid\":\"test\",\"password\":\"test-password\"},\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":4294967295,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":7,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":{\"hostname\":\"broker.example.test\",\"port\":8883,\"username\":\"device\",\"password\":\"secret\",\"ca_pem\":\"-----BEGIN CERTIFICATE-----\\nQQ==\\n-----END CERTIFICATE-----\",\"management_key_hex\":\"0100000000000000000000000000000000000000000000000000000000000000\"},\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":8,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":null,\"frp\":{\"server_hostname\":\"frp.example.test\",\"server_port\":7000,\"token\":\"test-frp-token\",\"ca_pem\":\"-----BEGIN CERTIFICATE-----\\nQQ==\\n-----END CERTIFICATE-----\",\"proxy_name\":\"base-device\",\"remote_port\":10200,\"local_port\":8123,\"management_key_hex\":\"0200000000000000000000000000000000000000000000000000000000000000\"},\"business\":null}}"
    };
    ebase_command_t out = {0};
    for (size_t i = 0; i < 4; ++i) {
        snprintf(json, sizeof json, "%s%s}", prefix, valid[i]);
        assert(!ebase_parse_command(json, strlen(json), &out, malloc) && out.kind == EBASE_CONFIG_SET);
        assert(out.config->revision == (i == 0 ? 0 : i == 1 ? UINT32_MAX : i == 2 ? 7 : 8));
        assert(out.config->wifi.configured == (i == 0));
        assert(out.config->mqtt.configured == (i == 2));
        if (i == 2) assert(out.config->mqtt.port == 8883 && out.config->mqtt.management_key[0] == 1);
        if (i == 3) assert(out.config->frp.configured && out.config->frp.server_port == 7000 &&
                           out.config->frp.local_port == 8123 && out.config->frp.management_key[0] == 2);
    }
    const char *bad[] = {
        "{}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":1,\"wifi\":null,\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":2,\"wifi\":null,\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":null,\"frp\":{},\"business\":null}}",
        "{\"expected_revision\":4294967296,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":true,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":{\"ssid\":\"test\",\"password\":\"short\"},\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":{\"ssid\":\"test\",\"ssid\":\"other\",\"password\":\"test-password\"},\"mqtt\":null,\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":{},\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":{\"hostname\":\"broker.example.test\",\"port\":8883,\"username\":\"device\",\"password\":\"secret\",\"ca_pem\":\"-----BEGIN CERTIFICATE-----\\nQQ==\\n-----END CERTIFICATE-----\",\"management_key_hex\":\"0000000000000000000000000000000000000000000000000000000000000000\"},\"frp\":null,\"business\":null}}",
        "{\"expected_revision\":0,\"config\":{\"schema_version\":3,\"wifi\":null,\"mqtt\":{\"hostname\":\"broker.example.test\",\"port\":8883,\"username\":\"device\",\"password\":\"secret\",\"ca_pem\":\"-----BEGIN CERTIFICATE-----\\nQQ==\\n-----END CERTIFICATE-----\",\"management_key_hex\":\"A100000000000000000000000000000000000000000000000000000000000000\"},\"frp\":null,\"business\":null}}"
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        snprintf(json, sizeof json, "%s%s}", prefix, bad[i]); reject(json);
    }
    /* A new command must not retain credentials from a previous parse. A
     * partial failure still keeps the request ID needed for its error reply. */
    snprintf(json, sizeof json, "%s%s}", prefix, valid[2]);
    assert(!ebase_parse_command(json, strlen(json), &out, malloc));
    assert(out.config->mqtt.configured && !strcmp(out.config->mqtt.password, "secret"));
    assert(!ebase_parse_command(status, sizeof status - 1, &out, malloc));
    assert(out.payload == NULL && out.payload_size_bytes == 0U);
    snprintf(json, sizeof json, "%s%s}", prefix, bad[10]);
    assert(ebase_parse_command(json, strlen(json), &out, malloc));
    assert(!strcmp(out.request.request_id, REQUEST));
    assert(out.payload == NULL && out.payload_size_bytes == 0U);
    ebase_command_release(&out);
}
static void ota_tests(void)
{
    char json[1400];
    const char *prefix = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.start\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":";
    const char *valid = "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/esp-base.bin\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}";
    ebase_command_t out = {0};
    snprintf(json, sizeof json, "%s%s}", prefix, valid);
    assert(!ebase_parse_command(json, strlen(json), &out, malloc) && out.kind == EBASE_OTA_START);
    assert(out.ota->image_size_bytes == 123456 && out.ota->sha256[0] == 0 && out.ota->sha256[31] == 31);
    assert(!strcmp(out.ota->image_url, "https://example.test/esp-base.bin"));
    assert(!out.ota->inbound_stream);
    const char *bad[] = {
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"http://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}",
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"esp32s3/esp_base\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}",
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":0,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}",
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"none\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        snprintf(json, sizeof json, "%s%s}", prefix, bad[i]); reject(json);
    }
    ebase_command_release(&out);
}
static void ota_result_tests(void)
{
    const char *valid = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\"}}";
    ebase_command_t out = {0};
    assert(!ebase_parse_command(valid, strlen(valid), &out, malloc) && out.kind == EBASE_OTA_RESULT);
    assert(!strcmp(out.operation_id, "44444444-4444-4444-8444-444444444444"));
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"bad\"}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"extra\":1}}");
    ebase_command_release(&out);
}
static void frp_status_tests(void)
{
    static const char valid[] =
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\","
        "\"request_id\":\"" REQUEST "\",\"command\":\"status\"}";
    ebase_request_t request;
    assert(!ebase_parse_frp_status(valid, sizeof valid - 1, &request));
    assert(!strcmp(request.device_id, DEVICE) && request.boot_id[0] == 0 &&
           !strcmp(request.request_id, REQUEST) && request.expires_at_ms == 0);
    assert(ebase_parse_frp_status(status, sizeof status - 1, &request));
    assert(request.request_id[0] == 0);
    static const char *invalid[] = {
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"request_id\":\"" REQUEST "\",\"command\":\"restart\"}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"parameters\":{}}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"request_id\":\"" REQUEST "\"}",
        "{\"protocol_version\":1,\"device_id\":\"bad-id\",\"request_id\":\"" REQUEST "\",\"command\":\"status\"}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"request_id\":\"bad-id\",\"command\":\"status\"}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\"}tail",
        /* The previous six-field contract is removed, even when valid. */
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"expires_at_uptime_ms\":31000}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000,\"request_id\":\"" REQUEST "\"}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":3.1}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000}tail"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        assert(ebase_parse_frp_status(invalid[i], strlen(invalid[i]), &request));
        const ebase_request_t empty = {0};
        assert(!memcmp(&request, &empty, sizeof request));
    }
    assert(ebase_parse_frp_status(valid, 513, &request));
}

static void frp_restart_tests(void)
{
    ebase_request_t request;
    ebase_command_t command = {0};
    assert(!ebase_parse_frp_restart(restart, strlen(restart), &request));
    assert(!ebase_parse_command(restart, strlen(restart), &command, malloc));
    assert(!memcmp(&request, &command.request, sizeof request));
    assert(ebase_parse_frp_restart(status, strlen(status), &request));
    const ebase_request_t empty = {0};
    assert(!memcmp(&request, &empty, sizeof empty));
    assert(ebase_parse_frp_restart(NULL, 0, &request));
    assert(ebase_parse_frp_restart(restart, 385, &request));
    assert(ebase_parse_frp_restart(restart, strlen(restart), NULL));
    char mutation[sizeof restart];
    unsigned seed = 142;
    for (unsigned i = 0; i < 10000; ++i) {
        memcpy(mutation, restart, sizeof restart);
        seed = seed * 1664525u + 1013904223u;
        mutation[seed % (sizeof restart - 1)] = (char)(seed >> 24);
        const char *frp_error = ebase_parse_frp_restart(mutation, sizeof restart - 1, &request);
        const char *error = ebase_parse_command(mutation, sizeof restart - 1, &command, malloc);
        assert((frp_error == NULL) == (error == NULL && command.kind == EBASE_RESTART));
        if (!frp_error) assert(!memcmp(&request, &command.request, sizeof request));
        else assert(!memcmp(&request, &empty, sizeof empty));
    }
    const char *invalid[] = {
        "{}", "[]", "null", "{\"command\":\"restart\"}",
        "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":null}",
        "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":{\"x\":1}}",
        "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000.0,\"parameters\":{}}",
        "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":-1,\"parameters\":{}}",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        assert(ebase_parse_frp_restart(invalid[i], strlen(invalid[i]), &request));
        assert(!memcmp(&request, &empty, sizeof empty));
    }
    ebase_command_release(&command);
}

static void native_firmware_frp_tests(void)
{
    char json[1400];
    ebase_command_t out = {0};
    const char *reads[] = {"firmware.status", "business.status"};
    const ebase_command_kind_t kinds[] = {EBASE_FIRMWARE_STATUS, EBASE_BUSINESS_STATUS};
    for (size_t i = 0; i < 2U; ++i) {
        snprintf(json, sizeof json, "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"%s\"}", reads[i]);
        assert(!ebase_parse_command(json, strlen(json), &out, malloc) && out.kind == kinds[i]);
        assert(out.payload == NULL);
        assert(ebase_parse_frp_command(json, strlen(json), &out, malloc));
        snprintf(json, sizeof json, "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"%s\",\"device_id\":\"" DEVICE "\"}", reads[i]);
        assert(!ebase_parse_frp_command(json, strlen(json), &out, malloc) && out.kind == kinds[i]);
        assert(!strcmp(out.request.device_id, DEVICE));
        assert(ebase_parse_command(json, strlen(json), &out, malloc));
    }
    const char *writes[] = {"business.pause", "business.resume"};
    const ebase_command_kind_t write_kinds[] = {EBASE_BUSINESS_PAUSE, EBASE_BUSINESS_RESUME};
    for (size_t i = 0; i < 2U; ++i) {
        snprintf(json, sizeof json, "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"%s\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}", writes[i]);
        assert(!ebase_parse_command(json, strlen(json), &out, malloc) && out.kind == write_kinds[i]);
        assert(!ebase_parse_frp_command(json, strlen(json), &out, malloc) && out.kind == write_kinds[i]);
        assert(out.payload == NULL && out.request.expires_at_ms == 31000U);
    }
    snprintf(json, sizeof json, "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.start\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}}");
    assert(!ebase_parse_frp_command(json, strlen(json), &out, malloc) && out.kind == EBASE_OTA_START);
    assert(out.ota->inbound_stream && out.ota->image_url[0] == '\0' && out.ota->image_size_bytes == 123456U);
    assert(ebase_parse_command(json, strlen(json), &out, malloc));
    const char *legacy[] = {"product.status", "product.install", "product.uninstall", "product.run.start", "product.run.stop", "product.run.event"};
    for (size_t i = 0; i < sizeof legacy / sizeof *legacy; ++i) {
        snprintf(json, sizeof json, "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"%s\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}", legacy[i]);
        reject(json);
        assert(ebase_parse_frp_command(json, strlen(json), &out, malloc));
    }
    ebase_command_release(&out);
}

int main(void)
{
    native_firmware_frp_tests();
    config_tests();
    ota_tests();
    ota_result_tests();
    frp_status_tests();
    frp_restart_tests();
    ebase_command_t out = {0};
    assert(!ebase_parse_command(status, strlen(status), &out, malloc) && out.kind == EBASE_STATUS);
    assert(!ebase_parse_command(restart, strlen(restart), &out, malloc) && out.kind == EBASE_RESTART);
    assert(!strcmp(out.request.device_id, DEVICE));
    assert(!strcmp(out.request.boot_id, BOOT));
    assert(out.request.expires_at_ms == 31000);
    ebase_request_guard_t guard = {0};
    size_t slot;
    assert(ebase_admit(&guard, &out.request, DEVICE, BOOT, 1000, &slot) == EBASE_ACCEPT);
    assert(ebase_admit(&guard, &out.request, DEVICE, BOOT, 1000, &slot) == EBASE_REPLAY);
    out.request.expires_at_ms++;
    assert(ebase_admit(&guard, &out.request, DEVICE, BOOT, 1000, &slot) == EBASE_REQUEST_CONFLICT);
    reject("{}"); reject("[]"); reject("null"); reject("{\"x\":true}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"command\":\"restart\"}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"comm\\u0061nd\":\"status\",\"command\":\"status\"}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"status\\u0000x\"}");
    reject("{\"protocol_version\":01,\"request_id\":\"" REQUEST "\",\"command\":\"status\"}");
    reject("{\"protocol_version\":1.0,\"request_id\":\"" REQUEST "\",\"command\":\"status\"}");
    reject("{\"protocol_version\":1e0,\"request_id\":\"" REQUEST "\",\"command\":\"status\"}");
    reject("{\"protocol_version\":true,\"request_id\":\"" REQUEST "\",\"command\":\"status\"}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"sta\xc0\x80tus\"}");
    char altered[1024];
    snprintf(altered, sizeof altered, "%s {}", status); reject(altered);
    for (size_t n = 0; n < strlen(status); ++n) assert(ebase_parse_command(status, n, &out, malloc));
    for (size_t split = 0; split <= strlen(status); ++split) {
        ebase_line_reader_t reader = {0};
        lines = invalid = 0;
        ebase_line_feed(&reader, status, split, receive, NULL);
        ebase_line_feed(&reader, status + split, strlen(status) - split, receive, NULL);
        assert(lines == 0);
        ebase_line_feed(&reader, "\n", 1, receive, NULL);
        assert(lines == 1 && invalid == 0);
    }
    ebase_line_reader_t reader = {0};
    char large[EBASE_LINE_LIMIT + 1]; memset(large, 'x', sizeof large);
    lines = invalid = 0;
    ebase_line_feed(&reader, large, sizeof large, receive, NULL);
    ebase_line_feed(&reader, status, strlen(status), receive, NULL);
    ebase_line_feed(&reader, "\n", 1, receive, NULL);
    assert(lines == 1 && invalid == 1);
    ebase_line_feed(&reader, status, strlen(status), receive, NULL);
    ebase_line_feed(&reader, "\n", 1, receive, NULL);
    assert(lines == 2 && invalid == 1);
    ebase_line_feed(&reader, "\0tail\n", 6, receive, NULL);
    assert(lines == 3 && invalid == 2);
    /* Deterministic malformed-input exercise through the real parser under ASan. */
    unsigned seed = 42;
    for (unsigned i = 0; i < 10000; ++i) {
        memcpy(altered, restart, sizeof restart);
        seed = seed * 1664525u + 1013904223u;
        altered[seed % (sizeof restart - 1)] = (char)(seed >> 24);
        (void)ebase_parse_command(altered, sizeof restart - 1, &out, malloc);
    }
    puts("  command_decoder  passed");
    ebase_command_release(&out);
}
