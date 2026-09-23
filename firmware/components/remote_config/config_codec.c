// SPDX-License-Identifier: Apache-2.0
#include "esp_base_config.h"
#include <string.h>

static bool zeros(const void *bytes, size_t count)
{
    const uint8_t *p = bytes;
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

static bool utf8(const uint8_t *s, size_t length, bool ssid)
{
    for (size_t i = 0; i < length;) {
        uint8_t c = s[i++];
        uint32_t cp = c, minimum = 0;
        unsigned more = 0;
        if (c >= 0xc2 && c <= 0xdf) { cp &= 31; more = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp &= 15; more = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp &= 7; more = 3; minimum = 0x10000; }
        else if (c >= 0x80) return false;
        if (more > length - i) return false;
        for (unsigned j = 0; j < more; ++j) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= (ssid ? 0x7f : 0x9f)) ||
            (!ssid && ((cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xffff) >= 0xfffe))) return false;
    }
    return true;
}

static bool wifi_values_valid(bool configured, const uint8_t *ssid, size_t ssid_len,
                              const uint8_t *password, size_t password_len)
{
    if (!configured) return ssid_len == 0 && password_len == 0;
    if (!ssid_len || ssid_len > 32 || !utf8(ssid, ssid_len, true) ||
        password_len < 8 || password_len > 64) return false;
    for (size_t i = 0; i < password_len; ++i) {
        const uint8_t c = password[i];
        if (password_len == 64) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        } else if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

bool ebase_wifi_config_valid(const ebase_wifi_config_t *wifi)
{
    size_t ssid, password;
    return wifi && text_length(wifi->ssid, sizeof wifi->ssid, &ssid) &&
        text_length(wifi->password, sizeof wifi->password, &password) &&
        wifi_values_valid(wifi->configured, (const uint8_t *)wifi->ssid, ssid,
                          (const uint8_t *)wifi->password, password);
}

static bool hostname_valid(const uint8_t *host, size_t length)
{
    if (!length || length > 253) return false;
    size_t label = 0;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = host[i];
        if (c == '.') {
            if (!label || host[i - 1] == '-' || i + 1 == length) return false;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || (c == '-' && label))) return false;
            if (++label > 63 || (i + 1 == length && c == '-')) return false;
        }
    }
    return true;
}

static bool contains(const uint8_t *haystack, size_t length, const char *needle)
{
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= length; ++i)
        if (!memcmp(haystack + i, needle, n)) return true;
    return false;
}

static bool mqtt_values_valid(bool configured, const uint8_t *host, size_t host_len,
                              uint16_t port, const uint8_t *user, size_t user_len,
                              const uint8_t *password, size_t password_len,
                              const uint8_t *ca, size_t ca_len, const uint8_t *key)
{
    if (!configured) return !host_len && !port && !user_len && !password_len &&
        !ca_len && zeros(key, EBASE_MQTT_KEY_BYTES);
    if (!hostname_valid(host, host_len) || !port || !user_len || user_len > 128 ||
        !password_len || password_len > 256 || !ca_len || ca_len > 4096 ||
        !utf8(user, user_len, false) || !utf8(password, password_len, false) ||
        zeros(key, EBASE_MQTT_KEY_BYTES) ||
        !contains(ca, ca_len, "-----BEGIN CERTIFICATE-----") ||
        !contains(ca, ca_len, "-----END CERTIFICATE-----")) return false;
    for (size_t i = 0; i < ca_len; ++i) {
        const uint8_t c = ca[i];
        if (!(c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7e))) return false;
    }
    return true;
}

static bool pem_valid(const uint8_t *ca, size_t length)
{
    if (!length || !contains(ca, length, "-----BEGIN CERTIFICATE-----") ||
        !contains(ca, length, "-----END CERTIFICATE-----")) return false;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = ca[i];
        if (!(c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7e))) return false;
    }
    return true;
}

static bool frp_values_valid(bool configured, const uint8_t *host, size_t host_len,
                             uint16_t server_port, const uint8_t *token, size_t token_len,
                             const uint8_t *ca, size_t ca_len, const uint8_t *proxy, size_t proxy_len,
                             uint16_t remote_port, uint16_t local_port, const uint8_t *key)
{
    if (!configured) return !host_len && !server_port && !token_len && !ca_len &&
        !proxy_len && !remote_port && !local_port && zeros(key, EBASE_FRP_KEY_BYTES);
    if (!hostname_valid(host, host_len) || !server_port || !token_len ||
        token_len > EBASE_FRP_TOKEN_MAX_BYTES || !ca_len || ca_len > EBASE_FRP_CA_MAX_BYTES ||
        !pem_valid(ca, ca_len) || !proxy_len || proxy_len > 128 ||
        !remote_port || !local_port || zeros(key, EBASE_FRP_KEY_BYTES)) return false;
    for (size_t i = 0; i < token_len; ++i)
        if (token[i] <= 0x20 || token[i] >= 0x7f) return false;
    for (size_t i = 0; i < proxy_len; ++i) {
        const uint8_t c = proxy[i];
        if (c <= 0x20 || c >= 0x7f || c == '/' || c == '\\' || c == '*' || c == '@') return false;
    }
    return true;
}

bool ebase_config_valid(const esp_base_remote_config_t *c)
{
    size_t host, user, password, ca, frp_host, token, frp_ca, proxy;
    const ebase_mqtt_config_t *m;
    if (!c || !ebase_wifi_config_valid(&c->wifi)) return false;
    m = &c->mqtt;
    const ebase_frp_config_t *f = &c->frp;
    return text_length(m->hostname, sizeof m->hostname, &host) &&
        text_length(m->username, sizeof m->username, &user) &&
        text_length(m->password, sizeof m->password, &password) &&
        text_length(m->ca_pem, sizeof m->ca_pem, &ca) &&
        mqtt_values_valid(m->configured, (const uint8_t *)m->hostname, host, m->port,
                          (const uint8_t *)m->username, user,
                          (const uint8_t *)m->password, password,
                          (const uint8_t *)m->ca_pem, ca, m->management_key) &&
        text_length(f->server_hostname, sizeof f->server_hostname, &frp_host) &&
        text_length(f->token, sizeof f->token, &token) &&
        text_length(f->ca_pem, sizeof f->ca_pem, &frp_ca) &&
        text_length(f->proxy_name, sizeof f->proxy_name, &proxy) &&
        frp_values_valid(f->configured, (const uint8_t *)f->server_hostname, frp_host,
                         f->server_port, (const uint8_t *)f->token, token,
                         (const uint8_t *)f->ca_pem, frp_ca,
                         (const uint8_t *)f->proxy_name, proxy,
                         f->remote_port, f->local_port, f->management_key);
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static uint16_t get16(const uint8_t *in)
{
    return (uint16_t)((uint16_t)in[0] | (uint16_t)in[1] << 8);
}

bool ebase_config_encode(const esp_base_remote_config_t *c,
                         uint8_t out[EBASE_CONFIG_MAX_BYTES], size_t *written)
{
    if (!out || !written || !ebase_config_valid(c)) return false;
    const size_t ssid = strlen(c->wifi.ssid), wifi_password = strlen(c->wifi.password);
    const size_t host = strlen(c->mqtt.hostname), user = strlen(c->mqtt.username);
    const size_t mqtt_password = strlen(c->mqtt.password), ca = strlen(c->mqtt.ca_pem);
    const size_t frp_host = strlen(c->frp.server_hostname), token = strlen(c->frp.token);
    const size_t frp_ca = strlen(c->frp.ca_pem), proxy = strlen(c->frp.proxy_name);
    memset(out, 0, EBASE_CONFIG_HEADER_BYTES);
    memcpy(out, "EBCF", 4);
    out[4] = 3;
    out[5] = (c->wifi.configured ? 1 : 0) | (c->mqtt.configured ? 2 : 0) |
        (c->frp.configured ? 4 : 0);
    out[6] = (uint8_t)ssid;
    out[7] = (uint8_t)wifi_password;
    for (unsigned i = 0; i < 4; ++i) out[8 + i] = (uint8_t)(c->revision >> (8 * i));
    out[12] = (uint8_t)host;
    out[13] = (uint8_t)user;
    put16(out + 14, (uint16_t)mqtt_password);
    put16(out + 16, (uint16_t)ca);
    put16(out + 18, c->mqtt.port);
    out[20] = (uint8_t)frp_host;
    out[21] = (uint8_t)proxy;
    put16(out + 22, (uint16_t)token);
    put16(out + 24, (uint16_t)frp_ca);
    put16(out + 26, c->frp.server_port);
    put16(out + 28, c->frp.remote_port);
    put16(out + 30, c->frp.local_port);
    size_t at = EBASE_CONFIG_HEADER_BYTES;
#define APPEND(bytes, length) do { memcpy(out + at, (bytes), (length)); at += (length); } while (0)
    APPEND(c->wifi.ssid, ssid);
    APPEND(c->wifi.password, wifi_password);
    APPEND(c->mqtt.hostname, host);
    APPEND(c->mqtt.username, user);
    APPEND(c->mqtt.password, mqtt_password);
    APPEND(c->mqtt.ca_pem, ca);
    if (c->mqtt.configured) APPEND(c->mqtt.management_key, EBASE_MQTT_KEY_BYTES);
    APPEND(c->frp.server_hostname, frp_host);
    APPEND(c->frp.token, token);
    APPEND(c->frp.ca_pem, frp_ca);
    APPEND(c->frp.proxy_name, proxy);
    if (c->frp.configured) APPEND(c->frp.management_key, EBASE_FRP_KEY_BYTES);
#undef APPEND
    *written = at;
    return true;
}

bool ebase_config_decode(const uint8_t *b, size_t length, esp_base_remote_config_t *out)
{
    if (!b || !out || length < EBASE_CONFIG_HEADER_BYTES || length > EBASE_CONFIG_MAX_BYTES ||
        memcmp(b, "EBCF", 4) || b[4] != 3 || (b[5] & ~7u) ||
        b[6] > 32 || b[7] > 64 || b[12] > 253 || b[13] > 128 ||
        get16(b + 14) > 256 || get16(b + 16) > 4096 ||
        b[20] > 253 || b[21] > 128 || get16(b + 22) > EBASE_FRP_TOKEN_MAX_BYTES ||
        get16(b + 24) > EBASE_FRP_CA_MAX_BYTES || !zeros(b + 32, 8)) return false;
    const size_t ssid = b[6], wifi_password = b[7], host = b[12], user = b[13];
    const size_t mqtt_password = get16(b + 14), ca = get16(b + 16);
    const size_t frp_host = b[20], proxy = b[21], token = get16(b + 22), frp_ca = get16(b + 24);
    const size_t expected = EBASE_CONFIG_HEADER_BYTES + ssid + wifi_password + host +
        user + mqtt_password + ca + ((b[5] & 2) ? EBASE_MQTT_KEY_BYTES : 0) +
        frp_host + token + frp_ca + proxy + ((b[5] & 4) ? EBASE_FRP_KEY_BYTES : 0);
    if (length != expected) return false;
    const uint8_t *ssid_bytes = b + EBASE_CONFIG_HEADER_BYTES;
    const uint8_t *wifi_password_bytes = ssid_bytes + ssid;
    const uint8_t *host_bytes = wifi_password_bytes + wifi_password;
    const uint8_t *user_bytes = host_bytes + host;
    const uint8_t *mqtt_password_bytes = user_bytes + user;
    const uint8_t *ca_bytes = mqtt_password_bytes + mqtt_password;
    const uint8_t *key_bytes = ca_bytes + ca;
    const uint8_t *frp_host_bytes = key_bytes + ((b[5] & 2) ? EBASE_MQTT_KEY_BYTES : 0);
    const uint8_t *token_bytes = frp_host_bytes + frp_host;
    const uint8_t *frp_ca_bytes = token_bytes + token;
    const uint8_t *proxy_bytes = frp_ca_bytes + frp_ca;
    const uint8_t *frp_key_bytes = proxy_bytes + proxy;
    static const uint8_t zero_key[EBASE_MQTT_KEY_BYTES] = {0};
    if (!wifi_values_valid((b[5] & 1) != 0, ssid_bytes, ssid, wifi_password_bytes, wifi_password) ||
        !mqtt_values_valid((b[5] & 2) != 0, host_bytes, host, get16(b + 18),
                           user_bytes, user, mqtt_password_bytes, mqtt_password,
                           ca_bytes, ca, (b[5] & 2) ? key_bytes : zero_key) ||
        !frp_values_valid((b[5] & 4) != 0, frp_host_bytes, frp_host, get16(b + 26),
                          token_bytes, token, frp_ca_bytes, frp_ca, proxy_bytes, proxy,
                          get16(b + 28), get16(b + 30), (b[5] & 4) ? frp_key_bytes : zero_key)) return false;
    memset(out, 0, sizeof *out);
    out->wifi.configured = (b[5] & 1) != 0;
    out->mqtt.configured = (b[5] & 2) != 0;
    out->frp.configured = (b[5] & 4) != 0;
    for (unsigned i = 0; i < 4; ++i) out->revision |= (uint32_t)b[8 + i] << (8 * i);
    memcpy(out->wifi.ssid, ssid_bytes, ssid);
    memcpy(out->wifi.password, wifi_password_bytes, wifi_password);
    memcpy(out->mqtt.hostname, host_bytes, host);
    out->mqtt.port = get16(b + 18);
    memcpy(out->mqtt.username, user_bytes, user);
    memcpy(out->mqtt.password, mqtt_password_bytes, mqtt_password);
    memcpy(out->mqtt.ca_pem, ca_bytes, ca);
    if (out->mqtt.configured) memcpy(out->mqtt.management_key, key_bytes, EBASE_MQTT_KEY_BYTES);
    memcpy(out->frp.server_hostname, frp_host_bytes, frp_host);
    out->frp.server_port = get16(b + 26);
    memcpy(out->frp.token, token_bytes, token);
    memcpy(out->frp.ca_pem, frp_ca_bytes, frp_ca);
    memcpy(out->frp.proxy_name, proxy_bytes, proxy);
    out->frp.remote_port = get16(b + 28);
    out->frp.local_port = get16(b + 30);
    if (out->frp.configured) memcpy(out->frp.management_key, frp_key_bytes, EBASE_FRP_KEY_BYTES);
    return true;
}
