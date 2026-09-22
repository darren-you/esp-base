// SPDX-License-Identifier: Apache-2.0
#include "esp_base_config.h"
#include <string.h>

static bool zeros(const void *bytes, size_t count)
{
    const unsigned char *p = bytes;
    for (size_t i = 0; i < count; ++i) if (p[i]) return false;
    return true;
}

static bool text_length(const char *text, size_t capacity, size_t *length)
{
    const char *end = memchr(text, 0, capacity);
    if (!end) return false;
    *length = (size_t)(end - text);
    return zeros(end, capacity - *length);
}

static bool ssid_valid(const unsigned char *s, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = s[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        uint32_t cp;
        unsigned count;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; count = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; count = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; count = 3; }
        else return false;
        if (i + count >= length) return false;
        for (unsigned j = 0; j < count; ++j) {
            c = s[++i];
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if ((count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

bool ebase_config_valid(const esp_base_remote_config_t *c)
{
    if (!c) return false;
    size_t ssid, password;
    if (!text_length(c->wifi.ssid, sizeof c->wifi.ssid, &ssid) ||
        !text_length(c->wifi.password, sizeof c->wifi.password, &password)) return false;
    if (!c->wifi.configured) return ssid == 0 && password == 0;
    if (ssid < 1 || ssid > 32 || !ssid_valid((const unsigned char *)c->wifi.ssid, ssid) ||
        password < 8 || password > 64) return false;
    for (size_t i = 0; i < password; ++i) {
        unsigned char ch = (unsigned char)c->wifi.password[i];
        if (password == 64) {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        } else if (ch < 0x20 || ch > 0x7e) return false;
    }
    return true;
}

bool ebase_config_encode(const esp_base_remote_config_t *c, uint8_t out[EBASE_CONFIG_BYTES])
{
    if (!out || !ebase_config_valid(c)) return false;
    memset(out, 0, EBASE_CONFIG_BYTES);
    memcpy(out, "EBCF", 4);
    out[4] = 1;
    out[5] = c->wifi.configured ? 1 : 0;
    out[6] = (uint8_t)strlen(c->wifi.ssid);
    out[7] = (uint8_t)strlen(c->wifi.password);
    for (unsigned i = 0; i < 4; ++i) out[8 + i] = (uint8_t)(c->revision >> (8 * i));
    memcpy(out + 16, c->wifi.ssid, out[6]);
    memcpy(out + 48, c->wifi.password, out[7]);
    return true;
}

bool ebase_config_decode(const uint8_t *bytes, size_t length, esp_base_remote_config_t *out)
{
    if (!bytes || !out || length != EBASE_CONFIG_BYTES || memcmp(bytes, "EBCF", 4) ||
        bytes[4] != 1 || bytes[5] > 1 || bytes[6] > 32 || bytes[7] > 64 ||
        !zeros(bytes + 12, 4) || !zeros(bytes + 16 + bytes[6], 32 - bytes[6]) ||
        !zeros(bytes + 48 + bytes[7], 64 - bytes[7])) return false;
    esp_base_remote_config_t c = {0};
    c.wifi.configured = bytes[5] == 1;
    for (unsigned i = 0; i < 4; ++i) c.revision |= (uint32_t)bytes[8 + i] << (8 * i);
    memcpy(c.wifi.ssid, bytes + 16, bytes[6]);
    memcpy(c.wifi.password, bytes + 48, bytes[7]);
    if (!ebase_config_valid(&c) || strlen(c.wifi.ssid) != bytes[6] || strlen(c.wifi.password) != bytes[7]) return false;
    *out = c;
    return true;
}
