// SPDX-License-Identifier: Apache-2.0
#include "esp_base_remote_config.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Faults exercise our storage caller, not NVS power-loss behavior. */
static uint8_t stored[EBASE_CONFIG_MAX_BYTES];
static size_t stored_size;
static unsigned writes, commits, handles;
static int fault;
static bool after_write;
enum { INIT_ERROR = 1, READ_ERROR, OPEN_ERROR, WRITE_ERROR_BEFORE,
       WRITE_ERROR_AFTER, COMMIT_ERROR, READBACK_ERROR, READBACK_MISMATCH };

esp_err_t nvs_flash_init_partition(const char *p)
{
    assert(!strcmp(p, "base_store"));
    return fault == INIT_ERROR ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *p, const char *ns, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(p, "base_store") && !strcmp(ns, "base_config"));
    if (mode == NVS_READWRITE && fault == OPEN_ERROR) return ESP_FAIL;
    if (!stored_size && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    ++handles; *handle = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 && handles > 0); --handles; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "committed"));
    if (fault == READ_ERROR || (fault == READBACK_ERROR && after_write)) return ESP_FAIL;
    if (!stored_size) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = stored_size; return ESP_OK; }
    if (*size < stored_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, stored, stored_size); *size = stored_size;
    if (fault == READBACK_MISMATCH && after_write) ((uint8_t *)out)[8] ^= 1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "committed") &&
           size >= EBASE_CONFIG_HEADER_BYTES && size <= sizeof stored);
    ++writes;
    if (fault == WRITE_ERROR_BEFORE) return ESP_FAIL;
    memcpy(stored, data, size); stored_size = size; after_write = true;
    return fault == WRITE_ERROR_AFTER ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && handles == 1); ++commits;
    return fault == COMMIT_ERROR ? ESP_FAIL : ESP_OK;
}

static esp_base_remote_config_t configured(uint32_t revision)
{
    esp_base_remote_config_t c = {.revision = revision, .wifi.configured = true};
    strcpy(c.wifi.ssid, "test-network");
    strcpy(c.wifi.password, "test-password");
    return c;
}
static void add_mqtt(esp_base_remote_config_t *c)
{
    c->mqtt.configured = true;
    strcpy(c->mqtt.hostname, "broker.example.test");
    c->mqtt.port = 8883;
    strcpy(c->mqtt.username, "device-user");
    strcpy(c->mqtt.password, "device-password");
    strcpy(c->mqtt.ca_pem, "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n");
    c->mqtt.management_key[0] = 0x01;
}
static void save(const esp_base_remote_config_t *config)
{
    assert(ebase_config_encode(config, stored, &stored_size));
    after_write = false;
}

static void codec_tests(void)
{
    esp_base_remote_config_t c = configured(0x12345678), read = {0};
    uint8_t bytes[EBASE_CONFIG_MAX_BYTES], changed[EBASE_CONFIG_MAX_BYTES];
    size_t size = 0;
    assert(ebase_config_encode(&c, bytes, &size));
    assert(size == EBASE_CONFIG_HEADER_BYTES + strlen(c.wifi.ssid) + strlen(c.wifi.password));
    assert(bytes[4] == 2 && bytes[8] == 0x78 && bytes[9] == 0x56 && bytes[10] == 0x34 && bytes[11] == 0x12);
    assert(ebase_config_decode(bytes, size, &read));
    assert(read.revision == c.revision && !strcmp(read.wifi.ssid, c.wifi.ssid) && !read.mqtt.configured);
    for (size_t n = 0; n < size; ++n) assert(!ebase_config_decode(bytes, n, &read));
    const unsigned invalid_offsets[] = {0, 4, 5, 6, 7, 12, 13, 14, 16, 18, 20};
    for (size_t i = 0; i < sizeof invalid_offsets / sizeof *invalid_offsets; ++i) {
        memcpy(changed, bytes, size); changed[invalid_offsets[i]] = 0xff;
        assert(!ebase_config_decode(changed, size, &read));
        assert(read.revision == c.revision);
    }
    memcpy(changed, bytes, size); changed[size] = 0;
    assert(!ebase_config_decode(changed, size + 1, &read));
    c.wifi.password[0] = 0; assert(!ebase_config_valid(&c));
    c = configured(0); memset(c.wifi.ssid, 'x', sizeof c.wifi.ssid); assert(!ebase_config_valid(&c));
    c = configured(0); c.wifi.ssid[0] = (char)0xc0; assert(!ebase_config_valid(&c));
    c = configured(0); c.wifi.configured = false; assert(!ebase_config_valid(&c));
    c = configured(0); memset(c.wifi.password, 'a', 64); c.wifi.password[64] = 0;
    assert(ebase_config_valid(&c));
    c.wifi.password[63] = 'z'; assert(!ebase_config_valid(&c));
    c = (esp_base_remote_config_t){0}; assert(ebase_config_valid(&c));
    assert(ebase_config_encode(&c, bytes, &size) && size == EBASE_CONFIG_HEADER_BYTES);
    assert(ebase_config_decode(bytes, size, &read) && !read.wifi.configured && !read.mqtt.configured);

    c = configured(7); add_mqtt(&c);
    assert(ebase_config_valid(&c));
    assert(ebase_config_encode(&c, bytes, &size));
    assert(bytes[5] == 3 && size > EBASE_CONFIG_HEADER_BYTES + EBASE_MQTT_KEY_BYTES);
    assert(ebase_config_decode(bytes, size, &read));
    assert(read.revision == 7 && read.mqtt.port == 8883 &&
           !strcmp(read.mqtt.hostname, c.mqtt.hostname) &&
           !memcmp(read.mqtt.management_key, c.mqtt.management_key, EBASE_MQTT_KEY_BYTES));
    memcpy(changed, bytes, size); memset(changed + size - EBASE_MQTT_KEY_BYTES, 0, EBASE_MQTT_KEY_BYTES);
    assert(!ebase_config_decode(changed, size, &read) && read.revision == 7);
    memcpy(changed, bytes, size); changed[21] = 1;
    assert(!ebase_config_decode(changed, size, &read));
    c.mqtt.management_key[0] = 0; assert(!ebase_config_valid(&c));
    add_mqtt(&c); c.mqtt.hostname[0] = '-'; assert(!ebase_config_valid(&c));
    add_mqtt(&c); c.mqtt.port = 0; assert(!ebase_config_valid(&c));
    add_mqtt(&c); c.mqtt.password[0] = (char)0xc0; assert(!ebase_config_valid(&c));
    add_mqtt(&c); c.mqtt.ca_pem[0] = 0; assert(!ebase_config_valid(&c));
    add_mqtt(&c); c.mqtt.configured = false; assert(!ebase_config_valid(&c));

    c = configured(8); add_mqtt(&c);
    for (unsigned i = 0; i < 253; ++i) c.mqtt.hostname[i] = i == 63 || i == 127 || i == 191 ? '.' : 'a';
    c.mqtt.hostname[253] = 0;
    memset(c.mqtt.username, 'u', 128); c.mqtt.username[128] = 0;
    memset(c.mqtt.password, 'p', 256); c.mqtt.password[256] = 0;
    memset(c.mqtt.ca_pem, 'A', 4096);
    memcpy(c.mqtt.ca_pem, "-----BEGIN CERTIFICATE-----", 27);
    memcpy(c.mqtt.ca_pem + 4096 - 25, "-----END CERTIFICATE-----", 25);
    c.mqtt.ca_pem[4096] = 0;
    memset(c.wifi.ssid, 's', 32); c.wifi.ssid[32] = 0;
    memset(c.wifi.password, 'a', 64); c.wifi.password[64] = 0;
    assert(ebase_config_valid(&c));
    assert(ebase_config_encode(&c, bytes, &size) && size == EBASE_CONFIG_MAX_BYTES);
    assert(ebase_config_decode(bytes, size, &read) && ebase_config_valid(&read));
}

int main(void)
{
    codec_tests();
    esp_base_remote_config_t current = configured(42), candidate = configured(0);
    assert(esp_base_remote_config_load(&current) == ESP_OK && current.revision == 0 && !current.wifi.configured);
    assert(esp_base_remote_config_commit_verified(&candidate, 0, &current) == ESP_OK);
    assert(current.revision == 1 && current.wifi.configured && writes == 1 && commits == 1);
    assert(esp_base_remote_config_commit_verified(&candidate, 0, &current) == ESP_BASE_CONFIG_CONFLICT && writes == 1);
    assert(esp_base_remote_config_load(&current) == ESP_OK && current.revision == 1);
    for (fault = INIT_ERROR; fault <= READBACK_MISMATCH; ++fault) {
        esp_base_remote_config_t baseline = configured(1);
        save(&baseline);
        candidate = configured(1); current = configured(99);
        unsigned before = writes;
        esp_err_t result = esp_base_remote_config_commit_verified(&candidate, 1, &current);
        if (fault <= OPEN_ERROR) { assert(result == ESP_FAIL && writes == before); }
        else { assert(result == ESP_BASE_CONFIG_UNCERTAIN && writes == before + 1); }
        assert(current.revision == 99 && handles == 0);
        if (fault == WRITE_ERROR_BEFORE) assert(stored[8] == 1);
        if (fault >= WRITE_ERROR_AFTER) assert(stored[8] == 2);
    }
    fault = 0; after_write = false;
    candidate = configured(UINT32_MAX); save(&candidate);
    unsigned before = writes;
    assert(esp_base_remote_config_commit_verified(&candidate, UINT32_MAX, &current) == ESP_BASE_CONFIG_EXHAUSTED && writes == before);
    stored[4] = 1;
    assert(esp_base_remote_config_load(&current) == ESP_ERR_INVALID_STATE && current.revision == 99);
    assert(esp_base_remote_config_commit_verified(&candidate, UINT32_MAX, &current) == ESP_ERR_INVALID_STATE && writes == before);
    memset(stored, 0, 112); memcpy(stored, "EBCF", 4); stored[4] = 1; stored_size = 112;
    assert(esp_base_remote_config_load(&current) == ESP_ERR_INVALID_STATE && current.revision == 99);
    candidate = configured(0);
    assert(esp_base_remote_config_commit_verified(&candidate, 0, &current) == ESP_ERR_INVALID_STATE && writes == before);
    stored_size = EBASE_CONFIG_MAX_BYTES + 1; /* Oversized key rejected before reading or writing. */
    assert(esp_base_remote_config_load(&current) == ESP_ERR_INVALID_STATE);
    assert(writes == before && handles == 0);
    puts("  config_store     passed (v2-only; fault injection, not a power-cut test)");
}
