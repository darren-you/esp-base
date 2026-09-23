// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

/* cJSON supplies the JSON tree. Before allocation, bound nesting and enforce
 * UTF-8, integer spelling and absence of embedded NUL (also escaped NUL). */
static bool valid_bytes(const unsigned char *s, size_t length)
{
    bool quoted = false;
    unsigned depth = 0, members = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = s[i];
        if (c == 0) return false;
        if (c >= 0x80) {
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
            if (!quoted) return false;
            continue;
        }
        if (quoted) {
            if (c < 0x20) return false;
            if (c == '"') quoted = false;
            else if (c == '\\') {
                if (++i == length) return false;
                if (s[i] == 'u' && i + 4 < length && !memcmp(s + i + 1, "0000", 4)) return false;
            }
        } else if (c == ':' || c == ',') { if (++members > 128) return false; }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 8) return false; }
        else if (c == '}' || c == ']') { if (depth == 0) return false; --depth; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            if (c == '-') { if (++i == length) return false; c = s[i]; }
            if (c < '0' || c > '9') return false;
            if (c == '0' && i + 1 < length && s[i + 1] >= '0' && s[i + 1] <= '9') return false;
            while (i + 1 < length && s[i + 1] >= '0' && s[i + 1] <= '9') ++i;
            if (i + 1 < length && (s[i + 1] == '.' || s[i + 1] == 'e' || s[i + 1] == 'E')) return false;
        } else if (c == '+' || c == '.' || (c < 0x20 && c != '\t' && c != '\r' && c != '\n')) return false;
    }
    return !quoted && depth == 0;
}

static bool exact_keys(const cJSON *object, const char *const *keys, size_t count)
{
    if (!cJSON_IsObject(object) || (size_t)cJSON_GetArraySize(object) != count) return false;
    for (size_t i = 0; i < count; ++i) {
        unsigned matches = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, object) if (item->string && !strcmp(item->string, keys[i])) ++matches;
        if (matches != 1) return false;
    }
    return true;
}

static bool copy_id(const cJSON *object, const char *key, char *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 36 || !ebase_is_uuid(item->valuestring)) return false;
    memcpy(out, item->valuestring, EBASE_ID_BYTES);
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

const char *ebase_parse_command(const char *json, size_t length, ebase_command_t *out)
{
    if (!out) return "invalid_request";
    memset(out, 0, sizeof *out);
    if (!json || !length || length > EBASE_LINE_LIMIT || !valid_bytes((const unsigned char *)json, length)) return "invalid_request";
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return "invalid_request";
    while (end < json + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    const char *error = "invalid_request";
    const char *const status_keys[] = {"protocol_version", "request_id", "command"};
    const char *const query_keys[] = {"protocol_version", "request_id", "command", "parameters"};
    const char *const write_keys[] = {"protocol_version", "request_id", "command", "device_id", "target_boot_id", "expires_at_uptime_ms", "parameters"};
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (end != json + length || !cJSON_IsObject(root) || !cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsString(command)) goto done;
    bool status = !strcmp(command->valuestring, "status");
    bool ota_result = !strcmp(command->valuestring, "ota.result");
    if (!exact_keys(root, status ? status_keys : ota_result ? query_keys : write_keys,
                    status ? 3 : ota_result ? 4 : 7)) goto done;
    if (!copy_id(root, "request_id", out->request.request_id)) goto done;
    if (status) { out->kind = EBASE_STATUS; error = NULL; goto done; }
    if (ota_result) {
        const char *const keys[] = {"operation_id"};
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
        if (!exact_keys(parameters, keys, 1) || !copy_id(parameters, "operation_id", out->operation_id)) goto done;
        out->kind = EBASE_OTA_RESULT;
        error = NULL;
        goto done;
    }
    if (!copy_id(root, "device_id", out->request.device_id) || !copy_id(root, "target_boot_id", out->request.boot_id)) goto done;
    const cJSON *deadline = cJSON_GetObjectItemCaseSensitive(root, "expires_at_uptime_ms");
    if (!cJSON_IsNumber(deadline) || !isfinite(deadline->valuedouble) || deadline->valuedouble < 0 ||
        deadline->valuedouble > 9007199254740991.0 || floor(deadline->valuedouble) != deadline->valuedouble) goto done;
    out->request.expires_at_ms = (uint64_t)deadline->valuedouble;
    if (!strcmp(command->valuestring, "ota.start")) {
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
        const char *const keys[] = {"operation_id", "image_url", "sha256", "image_size_bytes", "target", "signature"};
        if (!exact_keys(parameters, keys, 6) ||
            !copy_id(parameters, "operation_id", out->ota.operation_id)) goto done;
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(parameters, "image_url");
        const cJSON *digest = cJSON_GetObjectItemCaseSensitive(parameters, "sha256");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(parameters, "image_size_bytes");
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(parameters, "target");
        const cJSON *signature = cJSON_GetObjectItemCaseSensitive(parameters, "signature");
        const char *const signature_keys[] = {"scheme"};
        const cJSON *scheme = cJSON_GetObjectItemCaseSensitive(signature, "scheme");
        if (!cJSON_IsString(url) || strlen(url->valuestring) > ESP_BASE_OTA_URL_BYTES ||
            strncmp(url->valuestring, "https://", 8) != 0 ||
            !cJSON_IsString(digest) || strlen(digest->valuestring) != 64 ||
            !cJSON_IsNumber(size) || !isfinite(size->valuedouble) || size->valuedouble < 1 ||
            size->valuedouble > UINT32_MAX || floor(size->valuedouble) != size->valuedouble ||
            !cJSON_IsString(target) || strcmp(target->valuestring, ESP_BASE_OTA_TARGET) ||
            !exact_keys(signature, signature_keys, 1) || !cJSON_IsString(scheme) ||
            strcmp(scheme->valuestring, ESP_BASE_OTA_SIGNATURE_SCHEME)) goto done;
        for (size_t i = 0; i < sizeof out->ota.sha256; ++i) {
            const int hi = hex_digit(digest->valuestring[2 * i]);
            const int lo = hex_digit(digest->valuestring[2 * i + 1]);
            if (hi < 0 || lo < 0) goto done;
            out->ota.sha256[i] = (uint8_t)((hi << 4) | lo);
        }
        memcpy(out->ota.image_url, url->valuestring, strlen(url->valuestring) + 1);
        out->ota.image_size_bytes = (uint32_t)size->valuedouble;
        out->kind = EBASE_OTA_START;
        error = NULL;
        goto done;
    }
    if (!strcmp(command->valuestring, "config.set")) {
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
        const char *const parameter_keys[] = {"expected_revision", "config"};
        if (!exact_keys(parameters, parameter_keys, 2)) goto done;
        const cJSON *revision = cJSON_GetObjectItemCaseSensitive(parameters, "expected_revision");
        if (!cJSON_IsNumber(revision) || revision->valuedouble < 0 || revision->valuedouble > UINT32_MAX ||
            floor(revision->valuedouble) != revision->valuedouble) goto done;
        const cJSON *config = cJSON_GetObjectItemCaseSensitive(parameters, "config");
        const char *const config_keys[] = {"schema_version", "wifi", "mqtt", "frp", "business"};
        if (!exact_keys(config, config_keys, 5)) goto done;
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(config, "schema_version");
        if (!cJSON_IsNumber(schema) || schema->valuedouble != 1) goto done;
        if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(config, "mqtt")) ||
            !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(config, "frp")) ||
            !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(config, "business"))) {
            error = "unsupported_configuration"; goto done;
        }
        out->config.revision = (uint32_t)revision->valuedouble;
        const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(config, "wifi");
        if (!cJSON_IsNull(wifi)) {
            const char *const wifi_keys[] = {"ssid", "password"};
            if (!exact_keys(wifi, wifi_keys, 2)) goto done;
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
            const cJSON *password = cJSON_GetObjectItemCaseSensitive(wifi, "password");
            if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) > 32 ||
                !cJSON_IsString(password) || strlen(password->valuestring) > 64) goto done;
            out->config.wifi.configured = true;
            memcpy(out->config.wifi.ssid, ssid->valuestring, strlen(ssid->valuestring));
            memcpy(out->config.wifi.password, password->valuestring, strlen(password->valuestring));
        }
        if (!ebase_config_valid(&out->config)) goto done;
        out->kind = EBASE_CONFIG_SET;
        error = NULL;
        goto done;
    }
    if (strcmp(command->valuestring, "restart")) { error = "unsupported_command"; goto done; }
    if (!exact_keys(cJSON_GetObjectItemCaseSensitive(root, "parameters"), NULL, 0)) goto done;
    out->kind = EBASE_RESTART;
    /* All restart parameters are empty; IDs and deadline are compared by the
     * guard. SHA-256("restart") is the canonical command fingerprint. */
    const uint8_t fingerprint[32] = {
        0x3a, 0xce, 0x60, 0xb0, 0xa0, 0xc1, 0xb6, 0xc9, 0x34, 0x5e, 0x31, 0x49, 0x41, 0x42, 0x94, 0x7a, 0xa9, 0x7d, 0x4e, 0xf4, 0x91, 0x2e, 0x89, 0x5d, 0x87, 0x95, 0x66, 0x33, 0x93, 0x81, 0x97, 0x59
    };
    memcpy(out->request.fingerprint, fingerprint, sizeof fingerprint);
    error = NULL;
done:
    cJSON_Delete(root);
    return error;
}

void ebase_line_feed(ebase_line_reader_t *r, const void *bytes, size_t length,
                     ebase_line_handler_t handler, void *context)
{
    const unsigned char *input = bytes;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = input[i];
        if (c == '\n') {
            r->data[r->length] = '\0';
            if (r->discard || r->length) handler(r->discard ? NULL : r->data, r->discard ? 0 : r->length, context);
            r->length = 0;
            r->discard = false;
        } else if (!r->discard) {
            if (!c || r->length == EBASE_LINE_LIMIT) { r->length = 0; r->discard = true; }
            else r->data[r->length++] = (char)c;
        }
    }
}
