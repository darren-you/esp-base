// SPDX-License-Identifier: Apache-2.0
#include "esp_base_remote_config.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Faults exercise our storage caller, NOT an emulation of NVS power-loss behavior. */
static uint8_t stored[EBASE_CONFIG_BYTES];
static size_t stored_size;
static unsigned writes, commits, handles;
static int fault;
static bool after_write;
enum { INIT_ERROR = 1, READ_ERROR, OPEN_ERROR, WRITE_ERROR_BEFORE, WRITE_ERROR_AFTER, COMMIT_ERROR, READBACK_ERROR, READBACK_MISMATCH };
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
    if (*size < stored_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, stored, stored_size); *size = stored_size;
    if (fault == READBACK_MISMATCH && after_write) ((uint8_t *)out)[8] ^= 1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "committed") && size == sizeof stored);
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
    strcpy(c.wifi.ssid, "test-network"); strcpy(c.wifi.password, "test-password");
    return c;
}
static void codec_tests(void)
{
    esp_base_remote_config_t c = configured(0x12345678), read = {0};
    uint8_t bytes[EBASE_CONFIG_BYTES];
    assert(ebase_config_encode(&c, bytes));
    assert(bytes[8] == 0x78 && bytes[9] == 0x56 && bytes[10] == 0x34 && bytes[11] == 0x12);
    assert(ebase_config_decode(bytes, sizeof bytes, &read));
    assert(read.revision == c.revision && !strcmp(read.wifi.ssid, c.wifi.ssid));
    for (size_t n = 0; n < sizeof bytes; ++n) assert(!ebase_config_decode(bytes, n, &read));
    uint8_t changed[EBASE_CONFIG_BYTES];
    const unsigned invalid_offsets[] = {0, 4, 5, 6, 7, 12, 47, 111};
    for (size_t i = 0; i < sizeof invalid_offsets / sizeof *invalid_offsets; ++i) {
        memcpy(changed, bytes, sizeof bytes); changed[invalid_offsets[i]] = 0xff;
        assert(!ebase_config_decode(changed, sizeof changed, &read));
        assert(read.revision == c.revision); /* No partially decoded output. */
    }
    c.wifi.password[0] = 0; assert(!ebase_config_valid(&c));
    c = configured(0); memset(c.wifi.ssid, 'x', sizeof c.wifi.ssid); assert(!ebase_config_valid(&c));
    c = configured(0); c.wifi.ssid[0] = (char)0xc0; assert(!ebase_config_valid(&c));
    c = configured(0); c.wifi.configured = false; assert(!ebase_config_valid(&c));
    c = configured(0); memset(c.wifi.password, 'a', 64); c.wifi.password[64] = 0; assert(ebase_config_valid(&c));
    c.wifi.password[63] = 'z'; assert(!ebase_config_valid(&c));
    c = (esp_base_remote_config_t){0}; assert(ebase_config_valid(&c));
    assert(ebase_config_encode(&c, bytes) && ebase_config_decode(bytes, sizeof bytes, &read));
    assert(read.revision == 0 && !read.wifi.configured);
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
        assert(ebase_config_encode(&baseline, stored)); stored_size = sizeof stored; after_write = false;
        candidate = configured(1); current = configured(99);
        unsigned before = writes;
        esp_err_t result = esp_base_remote_config_commit_verified(&candidate, 1, &current);
        if (fault <= OPEN_ERROR) { assert(result == ESP_FAIL && writes == before); }
        else { assert(result == ESP_BASE_CONFIG_UNCERTAIN && writes == before + 1); }
        assert(current.revision == 99 && handles == 0);
        if (fault == WRITE_ERROR_BEFORE) assert(stored[8] == 1);
        if (fault >= WRITE_ERROR_AFTER) assert(stored[8] == 2); /* Failure does not promise rollback. */
    }
    fault = 0; after_write = false;
    candidate = configured(UINT32_MAX); assert(ebase_config_encode(&candidate, stored));
    unsigned before = writes;
    assert(esp_base_remote_config_commit_verified(&candidate, UINT32_MAX, &current) == ESP_BASE_CONFIG_EXHAUSTED && writes == before);
    stored[4] = 9;
    assert(esp_base_remote_config_load(&current) == ESP_ERR_INVALID_STATE && current.revision == 99);
    assert(esp_base_remote_config_commit_verified(&candidate, UINT32_MAX, &current) == ESP_ERR_INVALID_STATE && writes == before);
    assert(handles == 0);
    puts("  config_store     passed (fault injection; not a power-cut test)");
}
