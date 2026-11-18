// SPDX-License-Identifier: Apache-2.0
#include "base_protocol.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static bool is_lower_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool base_is_uuid(const char *text)
{
    if (!text || strlen(text) != BASE_UUID_BYTES - 1) return false;
    for (size_t i = 0; i < BASE_UUID_BYTES - 1; ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : !is_lower_hex(text[i])) return false;
    }
    return true;
}

bool base_is_sha256_hex(const char *text)
{
    if (!text || strlen(text) != BASE_SHA256_HEX_BYTES - 1) return false;
    for (size_t i = 0; i < BASE_SHA256_HEX_BYTES - 1; ++i) {
        if (!is_lower_hex(text[i])) return false;
    }
    return true;
}

bool base_topic(char out[BASE_TOPIC_BYTES], const char *device_id, base_topic_t topic)
{
    static const char *const suffix[] = {"status", "rsp", "cmd/+", "ota/desired", "ota/state"};
    if (!out || !base_is_uuid(device_id) || (unsigned)topic >= sizeof suffix / sizeof *suffix) return false;
    const int n = snprintf(out, BASE_TOPIC_BYTES, BASE_TOPIC_PREFIX "/%s/%s", device_id, suffix[topic]);
    return n > 0 && n < BASE_TOPIC_BYTES;
}

bool base_topic_command(const char *topic, size_t topic_len, const char *device_id,
                        char name[BASE_CMD_NAME_BYTES])
{
    char prefix[BASE_TOPIC_BYTES];
    if (!topic || !name || !base_is_uuid(device_id)) return false;
    const int n = snprintf(prefix, sizeof prefix, BASE_TOPIC_PREFIX "/%s/cmd/", device_id);
    if (n <= 0 || (size_t)n >= sizeof prefix || topic_len <= (size_t)n ||
        memcmp(topic, prefix, (size_t)n) != 0) return false;
    const size_t name_len = topic_len - (size_t)n;
    if (name_len >= BASE_CMD_NAME_BYTES) return false;
    for (size_t i = 0; i < name_len; ++i) {
        const char c = topic[(size_t)n + i];
        if (!(c >= 'a' && c <= 'z') && c != '_') return false;
    }
    memcpy(name, topic + n, name_len);
    name[name_len] = '\0';
    return true;
}

/* 取字符串字段到定长缓冲；缺失、类型不符、为空（除非允许）或超长都算失败。 */
static bool copy_string(const cJSON *object, const char *key, char *out, size_t size, bool allow_empty)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const size_t length = strlen(item->valuestring);
    if ((!allow_empty && length == 0) || length >= size) return false;
    memcpy(out, item->valuestring, length + 1);
    return true;
}

bool base_is_device_name(const char *name)
{
    if (!name) return false;
    const size_t length = strlen(name);
    if (length == 0 || length >= BASE_NAME_BYTES || name[0] == ' ' || name[length - 1] == ' ') return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

/* 只接受 wifi、business_mqtt、name 和 group；其他字段（包括旧协议改系统 MQTT 的 mqtt）一律拒绝，不部分执行。 */
static bool decode_config(const cJSON *root, base_config_update_t *out)
{
    static const char *const allowed[] = {"request_id", "wifi", "business_mqtt", "name", "group"};
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        bool known = false;
        for (size_t i = 0; i < sizeof allowed / sizeof *allowed; ++i) known = known || !strcmp(item->string, allowed[i]);
        if (!known) return false;
    }
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "business_mqtt");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group");
    if (wifi) {
        if (!cJSON_IsObject(wifi) ||
            !copy_string(wifi, "ssid", out->wifi_ssid, sizeof out->wifi_ssid, false) ||
            !copy_string(wifi, "password", out->wifi_password, sizeof out->wifi_password, false) ||
            strlen(out->wifi_password) < 8) return false;
        out->has_wifi = true;
    }
    if (mqtt) {
        base_mqtt_t *business = &out->business_mqtt;
        if (!cJSON_IsObject(mqtt) ||
            !copy_string(mqtt, "uri", business->uri, sizeof business->uri, false) ||
            !copy_string(mqtt, "username", business->username, sizeof business->username, false) ||
            !copy_string(mqtt, "password", business->password, sizeof business->password, false) ||
            strncmp(business->uri, "mqtts://", 8) != 0) return false;
        out->has_business_mqtt = true;
    }
    if (name) {
        if (!copy_string(root, "name", out->name, sizeof out->name, false) || !base_is_device_name(out->name)) return false;
        out->has_name = true;
    }
    if (group) {
        /* null 表示移出 Group。 */
        if (!cJSON_IsNull(group) &&
            (!copy_string(root, "group", out->group, sizeof out->group, false) || !base_is_uuid(out->group))) return false;
        out->has_group = true;
    }
    return out->has_wifi || out->has_business_mqtt || out->has_name || out->has_group;
}

bool base_cmd_decode(const char *name, const char *payload, size_t length,
                     base_cmd_t *out, const char **error)
{
    if (!name || !payload || !out || !error) return false;
    memset(out, 0, sizeof *out);
    *error = NULL;
    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (!cJSON_IsObject(root) ||
        !copy_string(root, "request_id", out->request_id, sizeof out->request_id, false) ||
        !base_is_uuid(out->request_id)) {
        out->request_id[0] = '\0';
        cJSON_Delete(root);
        return false;
    }
    bool ok = true;
    if (!strcmp(name, "status")) {
        out->kind = BASE_CMD_STATUS;
    } else if (!strcmp(name, "restart")) {
        out->kind = BASE_CMD_RESTART;
    } else if (!strcmp(name, "config")) {
        out->kind = BASE_CMD_CONFIG;
        ok = decode_config(root, &out->config);
        if (!ok) *error = "invalid_args";
    } else {
        ok = false;
        *error = "unknown_command";
    }
    cJSON_Delete(root);
    return ok;
}

int base_rsp_encode(char out[BASE_RSP_BYTES], const char *request_id, const char *cmd, const char *error)
{
    if (!out || !base_is_uuid(request_id) || !cmd) return -1;
    char quoted_error[BASE_ERROR_BYTES + 2] = "null";
    if (error) {
        const int e = snprintf(quoted_error, sizeof quoted_error, "\"%s\"", error);
        if (e <= 0 || (size_t)e >= sizeof quoted_error) return -1;
    }
    const int n = snprintf(out, BASE_RSP_BYTES,
        "{\"request_id\":\"%s\",\"cmd\":\"%s\",\"ok\":%s,\"error\":%s,\"result\":null}",
        request_id, cmd, error ? "false" : "true", quoted_error);
    return n > 0 && n < BASE_RSP_BYTES ? n : -1;
}

const char *base_dedupe_find(const base_dedupe_t *dedupe, const char *request_id)
{
    if (!dedupe || !request_id || !request_id[0]) return NULL;
    for (unsigned i = 0; i < BASE_DEDUPE_SLOTS; ++i) {
        if (!strcmp(dedupe->entries[i].request_id, request_id)) return dedupe->entries[i].response;
    }
    return NULL;
}

void base_dedupe_store(base_dedupe_t *dedupe, const char *request_id, const char *response)
{
    if (!dedupe || !base_is_uuid(request_id) || !response || strlen(response) >= BASE_RSP_BYTES) return;
    memcpy(dedupe->entries[dedupe->next].request_id, request_id, BASE_UUID_BYTES);
    strcpy(dedupe->entries[dedupe->next].response, response);
    dedupe->next = (dedupe->next + 1) % BASE_DEDUPE_SLOTS;
}

base_desired_result_t base_ota_desired_decode(const char *payload, size_t length,
                                              base_ota_desired_t *out, const char **error)
{
    if (!out || !error) return BASE_DESIRED_INVALID;
    memset(out, 0, sizeof *out);
    *error = NULL;
    if (!payload || length == 0) return BASE_DESIRED_EMPTY;
    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (!cJSON_IsObject(root) ||
        !copy_string(root, "job", out->job, sizeof out->job, false) || !base_is_uuid(out->job)) {
        out->job[0] = '\0';
        cJSON_Delete(root);
        return BASE_DESIRED_INVALID;
    }
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!copy_string(root, "version", out->version, sizeof out->version, false) ||
        !copy_string(root, "target", out->target, sizeof out->target, false) ||
        !copy_string(root, "sha256", out->sha256, sizeof out->sha256, false) ||
        !base_is_sha256_hex(out->sha256) ||
        !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > 0x7fffffff ||
        size->valuedouble != (double)(uint32_t)size->valuedouble) {
        *error = "invalid_desired";
    } else if (!copy_string(root, "url", out->url, sizeof out->url, false) ||
               strncmp(out->url, "https://", 8) != 0) {
        *error = "invalid_url";
    } else {
        out->size = (uint32_t)size->valuedouble;
    }
    cJSON_Delete(root);
    return *error ? BASE_DESIRED_REJECT : BASE_DESIRED_OK;
}

void base_default_name(char out[BASE_NAME_BYTES], const char *chip_model, const char *mac)
{
    /* esp32c3 → ESP32-C3，esp32 → ESP32。 */
    char chip[16] = "ESP";
    if (chip_model && chip_model[0]) {
        size_t n = 0;
        for (const char *c = chip_model; *c && n < sizeof chip - 2; ++c) {
            if (n == 5 && !strncmp(chip, "ESP32", 5)) chip[n++] = '-';
            chip[n++] = (char)toupper((unsigned char)*c);
        }
        chip[n] = '\0';
    }
    char tail[7] = "";
    const size_t length = mac ? strlen(mac) : 0;
    if (length == 17) {
        /* aa:bb:cc:dd:ee:ff 的后 3 字节。 */
        const char *last = mac + 9;
        snprintf(tail, sizeof tail, "%c%c%c%c%c%c", last[0], last[1], last[3], last[4], last[6], last[7]);
        for (char *c = tail; *c; ++c) *c = (char)toupper((unsigned char)*c);
    }
    snprintf(out, BASE_NAME_BYTES, tail[0] ? "%s-%s" : "%s", chip, tail);
}

/* 顺序写入定长缓冲；任何一步放不下都记为溢出。 */
typedef struct {
    char *out;
    size_t size, used;
    bool overflow;
} writer_t;

static void put(writer_t *w, const char *format, ...)
{
    if (w->overflow) return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(w->out + w->used, w->size - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->size - w->used) w->overflow = true;
    else w->used += (size_t)n;
}

static void put_char(writer_t *w, char c)
{
    if (w->overflow) return;
    if (w->used + 1 >= w->size) { w->overflow = true; return; }
    w->out[w->used++] = c;
    w->out[w->used] = '\0';
}

/* JSON 字符串：转义引号、反斜杠和控制字符；其余 UTF-8 原样写出。 */
static void put_string(writer_t *w, const char *value)
{
    put_char(w, '"');
    for (const unsigned char *c = (const unsigned char *)(value ? value : ""); *c && !w->overflow; ++c) {
        if (*c == '"' || *c == '\\') { put_char(w, '\\'); put_char(w, (char)*c); }
        else if (*c < 0x20 || *c == 0x7f) put(w, "\\u%04x", *c);
        else put_char(w, (char)*c);
    }
    put_char(w, '"');
}

static void put_nullable_string(writer_t *w, const char *value)
{
    if (value && value[0]) put_string(w, value);
    else put(w, "null");
}

static void put_identity(writer_t *w, const base_profile_t *p, bool online)
{
    char name[BASE_NAME_BYTES];
    if (p->name[0]) snprintf(name, sizeof name, "%s", p->name);
    else base_default_name(name, p->chip_model, p->mac);
    put(w, "{\"online\":%s,\"device_id\":", online ? "true" : "false");
    put_string(w, p->device_id);
    put(w, ",\"boot_id\":");
    put_string(w, p->boot_id);
    if (online) {
        put(w, ",\"uptime_s\":%" PRIu32 ",\"reset\":", p->uptime_s);
        put_string(w, p->reset);
    }
    put(w, ",\"name\":");
    put_string(w, name);
    put(w, ",\"group\":");
    put_nullable_string(w, p->group);
    put(w, ",\"fw\":{\"version\":");
    put_string(w, p->fw.version);
    put(w, ",\"sha256\":");
    put_string(w, p->fw.sha256);
    put(w, ",\"target\":");
    put_string(w, p->fw.target);
    put(w, ",\"slot\":");
    put_string(w, p->fw.slot);
    put(w, "}");
}

static int finish(writer_t *w)
{
    return w->overflow ? -1 : (int)w->used;
}

int base_will_encode(const base_profile_t *profile, char *out, size_t size)
{
    if (!profile || !out || size == 0) return -1;
    writer_t w = {.out = out, .size = size};
    put_identity(&w, profile, false);
    put(&w, "}");
    return finish(&w);
}

int base_status_encode(const base_profile_t *p, char *out, size_t size)
{
    if (!p || !out || size == 0) return -1;
    static const struct { uint32_t bit; const char *name; } features[] = {
        {BASE_CHIP_WIFI, "wifi"}, {BASE_CHIP_BLE, "ble"}, {BASE_CHIP_BT, "bt"},
        {BASE_CHIP_IEEE802154, "ieee802154"}, {BASE_CHIP_EMB_FLASH, "emb_flash"}, {BASE_CHIP_EMB_PSRAM, "emb_psram"},
    };
    writer_t w = {.out = out, .size = size};
    put_identity(&w, p, true);
    put(&w, ",\"chip\":{\"model\":");
    put_string(&w, p->chip_model);
    put(&w, ",\"revision\":");
    put_string(&w, p->chip_revision);
    put(&w, ",\"cores\":%u,\"cpu_mhz\":%u,\"features\":[", (unsigned)p->cores, (unsigned)p->cpu_mhz);
    bool first = true;
    for (size_t i = 0; i < sizeof features / sizeof *features; ++i) {
        if (!(p->chip_features & features[i].bit)) continue;
        put(&w, "%s\"%s\"", first ? "" : ",", features[i].name);
        first = false;
    }
    put(&w, "],\"flash\":%" PRIu32 ",\"psram\":%" PRIu32 "},\"idf\":", p->flash_bytes, p->psram_bytes);
    put_string(&w, p->idf);
    put(&w, ",\"heap\":{\"free\":%" PRIu32 ",\"min\":%" PRIu32 ",\"largest\":%" PRIu32 "},\"stack\":{",
        p->heap_free, p->heap_min, p->heap_largest);
    for (unsigned i = 0; i < p->stack_count && i < BASE_STACK_SLOTS; ++i) {
        if (i) put(&w, ",");
        put_string(&w, p->stack[i].name);
        put(&w, ":%" PRIu32, p->stack[i].bytes);
    }
    put(&w, "},\"wifi\":{\"rssi\":%d,\"ssid\":", (int)p->rssi);
    put_string(&w, p->ssid);
    put(&w, ",\"bssid\":");
    put_string(&w, p->bssid);
    put(&w, ",\"channel\":%u},\"net\":{\"mac\":", (unsigned)p->channel);
    put_string(&w, p->mac);
    put(&w, ",\"hostname\":");
    put_string(&w, p->hostname);
    put(&w, ",\"ipv4\":");
    if (p->has_ipv4) {
        put(&w, "{\"address\":");
        put_string(&w, p->ipv4);
        put(&w, ",\"netmask\":");
        put_string(&w, p->netmask);
        put(&w, ",\"gateway\":");
        put_string(&w, p->gateway);
        put(&w, "}");
    } else {
        put(&w, "null");
    }
    put(&w, ",\"ipv6\":[");
    for (unsigned i = 0; i < p->ipv6_count && i < BASE_IPV6_SLOTS; ++i) {
        if (i) put(&w, ",");
        put_string(&w, p->ipv6[i]);
    }
    put(&w, "]},\"mqtt\":{\"uri\":");
    put_string(&w, p->mqtt_uri);
    put(&w, "},\"business_mqtt\":");
    if (p->business_mqtt_uri[0]) {
        put(&w, "{\"uri\":");
        put_string(&w, p->business_mqtt_uri);
        put(&w, "}");
    } else {
        put(&w, "null");
    }
    put(&w, ",\"config\":{\"revision\":%" PRIu32 ",\"error\":", p->config_revision);
    put_nullable_string(&w, p->config_error);
    put(&w, "},\"wakeups\":%" PRIu32 "}", p->wakeups);
    return finish(&w);
}
