// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command.h"
#include "esp_base_ota_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define REQUEST "11111111-1111-4111-8111-111111111111"
#define DEVICE "22222222-2222-4222-8222-222222222222"
#define BOOT "33333333-3333-4333-8333-333333333333"
static const char status[] = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"status\"}";
static const char restart[] = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}";
static void reject(const char *json)
{
    ebase_command_t out;
    assert(ebase_parse_command(json, strlen(json), &out) != NULL);
}
static unsigned lines, invalid;
static void receive(const char *line, size_t length, void *context)
{
    (void)context;
    ++lines;
    if (!line) { ++invalid; return; }
    ebase_command_t out;
    assert(!ebase_parse_command(line, length, &out));
    assert(out.kind == EBASE_STATUS);
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
    ebase_command_t out;
    for (size_t i = 0; i < 4; ++i) {
        snprintf(json, sizeof json, "%s%s}", prefix, valid[i]);
        assert(!ebase_parse_command(json, strlen(json), &out) && out.kind == EBASE_CONFIG_SET);
        assert(out.config.revision == (i == 0 ? 0 : i == 1 ? UINT32_MAX : i == 2 ? 7 : 8));
        assert(out.config.wifi.configured == (i == 0));
        assert(out.config.mqtt.configured == (i == 2));
        if (i == 2) assert(out.config.mqtt.port == 8883 && out.config.mqtt.management_key[0] == 1);
        if (i == 3) assert(out.config.frp.configured && out.config.frp.server_port == 7000 &&
                           out.config.frp.local_port == 8123 && out.config.frp.management_key[0] == 2);
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
    const unsigned char cleared[sizeof out.config] = {0};
    snprintf(json, sizeof json, "%s%s}", prefix, valid[2]);
    assert(!ebase_parse_command(json, strlen(json), &out));
    assert(out.config.mqtt.configured && !strcmp(out.config.mqtt.password, "secret"));
    assert(!ebase_parse_command(status, sizeof status - 1, &out));
    assert(!memcmp(&out.config, cleared, sizeof cleared));
    snprintf(json, sizeof json, "%s%s}", prefix, bad[10]);
    assert(ebase_parse_command(json, strlen(json), &out));
    assert(!strcmp(out.request.request_id, REQUEST));
    assert(!memcmp(&out.config, cleared, sizeof cleared));
}
static void ota_tests(void)
{
    char json[1400];
    const char *prefix = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.start\",\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,\"parameters\":";
    const char *valid = "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/esp-base.bin\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}";
    ebase_command_t out;
    snprintf(json, sizeof json, "%s%s}", prefix, valid);
    assert(!ebase_parse_command(json, strlen(json), &out) && out.kind == EBASE_OTA_START);
    assert(out.ota.image_size_bytes == 123456 && out.ota.sha256[0] == 0 && out.ota.sha256[31] == 31);
    assert(!strcmp(out.ota.image_url, "https://example.test/esp-base.bin"));
    const char *bad[] = {
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"http://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}",
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"esp32s3/esp_base\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}",
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":0,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"" ESP_BASE_OTA_SIGNATURE_SCHEME "\"}}",
        "{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"image_url\":\"https://example.test/a\",\"sha256\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"image_size_bytes\":123456,\"target\":\"" ESP_BASE_OTA_TARGET "\",\"signature\":{\"scheme\":\"none\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        snprintf(json, sizeof json, "%s%s}", prefix, bad[i]); reject(json);
    }
}
static void ota_result_tests(void)
{
    const char *valid = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\"}}";
    ebase_command_t out;
    assert(!ebase_parse_command(valid, strlen(valid), &out) && out.kind == EBASE_OTA_RESULT);
    assert(!strcmp(out.operation_id, "44444444-4444-4444-8444-444444444444"));
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"bad\"}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"ota.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"extra\":1}}");
}
static void product_result_tests(void)
{
    const char *valid = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\"}}";
    ebase_command_t out;
    assert(!ebase_parse_command(valid, strlen(valid), &out) &&
           out.kind == EBASE_PRODUCT_RESULT);
    assert(!strcmp(out.operation_id, "44444444-4444-4444-8444-444444444444"));
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.result\",\"parameters\":{}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.result\",\"parameters\":{\"operation_id\":\"bad\"}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.result\",\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\",\"extra\":1}}");
}
static void product_status_tests(void)
{
    const char *valid = "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.status\"}";
    ebase_command_t out;
    assert(!ebase_parse_command(valid, strlen(valid), &out) &&
           out.kind == EBASE_PRODUCT_STATUS);
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.status\",\"parameters\":{}}");
    reject("{\"protocol_version\":1,\"request_id\":\"" REQUEST "\",\"command\":\"product.status\",\"extra\":1}");
}
static void product_uninstall_tests(void)
{
    const char *format =
        "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\","
        "\"command\":\"product.uninstall\",\"device_id\":\"" DEVICE "\","
        "\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,"
        "\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\","
        "\"operation_sequence\":%s,\"expected_container_sequence\":%s,"
        "\"package_sha256\":\"%s\"}}";
    char json[600];
    char digest[65];
    memset(digest, 'a', 64);
    digest[64] = '\0';
    assert(snprintf(json, sizeof json, format, "1", "6", digest) > 0);
    ebase_command_t out;
    assert(!ebase_parse_command(json, strlen(json), &out) &&
           out.kind == EBASE_PRODUCT_UNINSTALL_COMMAND &&
           out.product_uninstall.operation_sequence == 1U &&
           out.product_uninstall.expected_container_sequence == 6U &&
           out.product_uninstall.package_sha256[0] == 0xaaU);
    assert(snprintf(json, sizeof json, format, "0", "6", digest) > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "1", "4294967295", digest) > 0);
    reject(json);
    memset(digest, '0', 64);
    assert(snprintf(json, sizeof json, format, "1", "6", digest) > 0);
    reject(json);
    digest[0] = 'A';
    assert(snprintf(json, sizeof json, format, "1", "6", digest) > 0);
    reject(json);
}
static void product_package_tests(void)
{
    const char *format =
        "{\"protocol_version\":1,\"request_id\":\"" REQUEST "\","
        "\"command\":\"product.%s\",\"device_id\":\"" DEVICE "\","
        "\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":31000,"
        "\"parameters\":{\"operation_id\":\"44444444-4444-4444-8444-444444444444\","
        "\"operation_sequence\":%s,\"expected_container_sequence\":%s,"
        "\"previous_package_sha256\":%s,\"package_url\":\"%s\","
        "\"package_sha256\":\"%s\",\"package_size_bytes\":%s,"
        "\"guest_abi_version\":%s,\"data_schema_version\":%s}}";
    const char *digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char json[2300];
    ebase_command_t out;
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "https://packages.example.test/app.pkg", digest, "10240", "2", "1") > 0);
    assert(!ebase_parse_command(json, strlen(json), &out) &&
        out.kind == EBASE_PRODUCT_INSTALL_COMMAND &&
        !out.product_package.previous_package_present &&
        out.product_package.operation_sequence == 1U &&
        out.product_package.expected_container_sequence == 6U &&
        out.product_package.package_size_bytes == 10240U &&
        out.product_package.guest_abi_version == 2U &&
        out.product_package.data_schema_version == 1U &&
        out.product_package.package_sha256[0] == 0xaaU);
    assert(snprintf(json, sizeof json, format, "upgrade", "2", "11", "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"",
        "https://packages.example.test/app-v2.pkg", digest, "10240", "2", "1") > 0);
    assert(!ebase_parse_command(json, strlen(json), &out) &&
        out.kind == EBASE_PRODUCT_UPGRADE_COMMAND &&
        out.product_package.previous_package_present &&
        out.product_package.previous_package_sha256[0] == 0xbbU);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"",
        "https://packages.example.test/app.pkg", digest, "10240", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "upgrade", "2", "11", "null",
        "https://packages.example.test/app.pkg", digest, "10240", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "0", "6", "null",
        "https://packages.example.test/app.pkg", digest, "10240", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "4294967291", "null",
        "https://packages.example.test/app.pkg", digest, "10240", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "http://packages.example.test/app.pkg", digest, "10240", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "https://packages.example.test/app.pkg", digest, "0", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "https://packages.example.test/app.pkg", digest, "10240", "0", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "https://packages.example.test/app.pkg", digest, "10240", "2", "0") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "https://packages.example.test/app.pkg", digest, "2147483648", "2", "1") > 0);
    reject(json);
    assert(snprintf(json, sizeof json, format, "install", "1", "6", "null",
        "https://packages.example.test/app.pkg", "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "10240", "2", "1") > 0);
    reject(json);
}
static void frp_status_tests(void)
{
    static const char valid[] =
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\","
        "\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\","
        "\"command\":\"status\",\"expires_at_uptime_ms\":31000}";
    ebase_request_t request;
    assert(!ebase_parse_frp_status(valid, sizeof valid - 1, &request));
    assert(!strcmp(request.device_id, DEVICE) && !strcmp(request.boot_id, BOOT) &&
           !strcmp(request.request_id, REQUEST) && request.expires_at_ms == 31000);
    assert(ebase_parse_frp_status(status, sizeof status - 1, &request));
    assert(request.request_id[0] == 0);
    static const char *invalid[] = {
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"restart\",\"expires_at_uptime_ms\":31000}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000,\"request_id\":\"" REQUEST "\"}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":3.1}",
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"target_boot_id\":\"" BOOT "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\",\"expires_at_uptime_ms\":31000}tail"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        assert(ebase_parse_frp_status(invalid[i], strlen(invalid[i]), &request));
    assert(ebase_parse_frp_status(valid, 513, &request));
}
int main(void)
{
    config_tests();
    ota_tests();
    ota_result_tests();
    product_result_tests();
    product_status_tests();
    product_uninstall_tests();
    product_package_tests();
    frp_status_tests();
    ebase_command_t out;
    assert(!ebase_parse_command(status, strlen(status), &out) && out.kind == EBASE_STATUS);
    assert(!ebase_parse_command(restart, strlen(restart), &out) && out.kind == EBASE_RESTART);
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
    for (size_t n = 0; n < strlen(status); ++n) assert(ebase_parse_command(status, n, &out));
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
        (void)ebase_parse_command(altered, sizeof restart - 1, &out);
    }
    puts("  command_decoder  passed");
}
