// SPDX-License-Identifier: Apache-2.0
#include "base_protocol.h"

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

static bool decode_config(const cJSON *root, base_config_update_t *out)
{
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (wifi) {
        if (!cJSON_IsObject(wifi) ||
            !copy_string(wifi, "ssid", out->wifi_ssid, sizeof out->wifi_ssid, false) ||
            !copy_string(wifi, "password", out->wifi_password, sizeof out->wifi_password, false) ||
            strlen(out->wifi_password) < 8) return false;
        out->has_wifi = true;
    }
    if (mqtt) {
        if (!cJSON_IsObject(mqtt) ||
            !copy_string(mqtt, "uri", out->mqtt_uri, sizeof out->mqtt_uri, false) ||
            !copy_string(mqtt, "username", out->mqtt_username, sizeof out->mqtt_username, false) ||
            !copy_string(mqtt, "password", out->mqtt_password, sizeof out->mqtt_password, false) ||
            strncmp(out->mqtt_uri, "mqtts://", 8) != 0) return false;
        out->has_mqtt = true;
    }
    return out->has_wifi || out->has_mqtt;
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
    } else if (!strcmp(name, "business")) {
        out->kind = BASE_CMD_BUSINESS;
        ok = copy_string(root, "op", out->business_op, sizeof out->business_op, false);
        if (ok && !strcmp(out->business_op, "add")) {
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
            ok = cJSON_IsString(data) && data->valuestring && data->valuestring[0] != '\0';
            if (ok) out->business_bytes = (uint32_t)strlen(data->valuestring);
        }
        if (!ok) *error = "invalid_args";
    } else {
        ok = false;
        *error = "unknown_command";
    }
    cJSON_Delete(root);
    return ok;
}

int base_rsp_encode(char out[BASE_RSP_BYTES], const char *request_id, const char *cmd,
                    const char *error, const char *result_json)
{
    if (!out || !base_is_uuid(request_id) || !cmd) return -1;
    char quoted_error[BASE_ERROR_BYTES + 2] = "null";
    if (error) {
        const int e = snprintf(quoted_error, sizeof quoted_error, "\"%s\"", error);
        if (e <= 0 || (size_t)e >= sizeof quoted_error) return -1;
    }
    const int n = snprintf(out, BASE_RSP_BYTES,
        "{\"request_id\":\"%s\",\"cmd\":\"%s\",\"ok\":%s,\"error\":%s,\"result\":%s}",
        request_id, cmd, error ? "false" : "true", quoted_error, result_json ? result_json : "null");
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

const char *base_business_apply(base_business_t *business, const char *op, uint32_t data_bytes)
{
    if (!business || !op) return "invalid_args";
    if (!strcmp(op, "add")) {
        if (business->paused) return "paused";
        if (data_bytes == 0) return "invalid_args";
        if (data_bytes > UINT32_MAX - business->bytes) return "overflow";
        business->bytes += data_bytes;
        return NULL;
    }
    if (!strcmp(op, "pause")) {
        business->paused = true;
        return NULL;
    }
    if (!strcmp(op, "resume")) {
        business->paused = false;
        return NULL;
    }
    return "invalid_args";
}

int base_business_encode(const base_business_t *business, char *out, size_t size)
{
    if (!business || !out) return -1;
    const int n = snprintf(out, size, "{\"state\":\"%s\",\"bytes\":%lu}",
                           business->paused ? "paused" : "active", (unsigned long)business->bytes);
    return n > 0 && (size_t)n < size ? n : -1;
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
