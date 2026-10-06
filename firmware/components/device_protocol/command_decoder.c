// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

static void release_payload(ebase_command_t *command)
{
    volatile unsigned char *bytes = command->payload;
    for (size_t i = 0; i < command->payload_size_bytes; ++i) bytes[i] = 0;
    free(command->payload);
    command->payload = NULL;
    command->payload_size_bytes = 0U;
}

void ebase_command_release(ebase_command_t *command)
{
    if (!command) return;
    release_payload(command);
    memset(command, 0, sizeof *command);
}

static bool allocate_payload(ebase_command_t *command, size_t size_bytes,
                             ebase_command_alloc_t allocate)
{
    command->payload = allocate(size_bytes);
    if (!command->payload) return false;
    command->payload_size_bytes = size_bytes;
    memset(command->payload, 0, size_bytes);
    return true;
}

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

static bool digest32(const cJSON *item, uint8_t output[32])
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 64U) return false;
    uint8_t nonzero = 0U;
    for (size_t index = 0; index < 32U; ++index) {
        const int upper = hex_digit(item->valuestring[index * 2U]);
        const int lower = hex_digit(item->valuestring[index * 2U + 1U]);
        if (upper < 0 || lower < 0) return false;
        output[index] = (uint8_t)((upper << 4) | lower);
        nonzero |= output[index];
    }
    return nonzero != 0U;
}

static const char *const write_keys[] = {"protocol_version", "request_id", "command",
    "device_id", "target_boot_id", "expires_at_uptime_ms", "parameters"};

static bool write_identity(const cJSON *root, ebase_request_t *request)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *deadline = cJSON_GetObjectItemCaseSensitive(root, "expires_at_uptime_ms");
    if (!exact_keys(root, write_keys, 7) || !cJSON_IsNumber(version) ||
        version->valuedouble != 1 || !copy_id(root, "request_id", request->request_id) ||
        !copy_id(root, "device_id", request->device_id) ||
        !copy_id(root, "target_boot_id", request->boot_id) ||
        !cJSON_IsNumber(deadline) || !isfinite(deadline->valuedouble) ||
        deadline->valuedouble < 0 || deadline->valuedouble > 9007199254740991.0 ||
        floor(deadline->valuedouble) != deadline->valuedouble) return false;
    request->expires_at_ms = (uint64_t)deadline->valuedouble;
    return true;
}

static bool restart_parameters(const cJSON *root, ebase_request_t *request)
{
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(command) || strcmp(command->valuestring, "restart") ||
        !exact_keys(cJSON_GetObjectItemCaseSensitive(root, "parameters"), NULL, 0)) return false;
    /* SHA-256("restart"); identity and deadline are compared by the guard. */
    static const uint8_t fingerprint[32] = {
        0x3a, 0xce, 0x60, 0xb0, 0xa0, 0xc1, 0xb6, 0xc9, 0x34, 0x5e, 0x31, 0x49,
        0x41, 0x42, 0x94, 0x7a, 0xa9, 0x7d, 0x4e, 0xf4, 0x91, 0x2e, 0x89, 0x5d,
        0x87, 0x95, 0x66, 0x33, 0x93, 0x81, 0x97, 0x59
    };
    memcpy(request->fingerprint, fingerprint, sizeof fingerprint);
    return true;
}

static const char *parse_command(const char *json, size_t length,
                                ebase_command_t *out, ebase_command_alloc_t allocate, bool frp)
{
    if (!out) return "invalid_request";
    ebase_command_release(out);
    if (!allocate) return "invalid_request";
    if (!json || !length || length > EBASE_LINE_LIMIT || !valid_bytes((const unsigned char *)json, length)) return "invalid_request";
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return "invalid_request";
    while (end < json + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    const char *error = "invalid_request";
    const char *const status_keys[] = {"protocol_version", "request_id", "command"};
    const char *const query_keys[] = {"protocol_version", "request_id", "command", "parameters"};
    const char *const frp_read_keys[] = {"protocol_version", "request_id", "command", "device_id"};
    const char *const frp_query_keys[] = {"protocol_version", "request_id", "command", "parameters", "device_id"};
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (end != json + length || !cJSON_IsObject(root) || !cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsString(command)) goto done;
    bool status = !strcmp(command->valuestring, "status");
    bool firmware_status = !strcmp(command->valuestring, "firmware.status");
    bool business_status = !strcmp(command->valuestring, "business.status");
    bool ota_result = !strcmp(command->valuestring, "ota.result");
    const bool query = ota_result;
    const bool read = status || firmware_status || business_status;
    if (!exact_keys(root, read ? (frp ? frp_read_keys : status_keys) :
                    query ? (frp ? frp_query_keys : query_keys) : write_keys,
                    read ? (frp ? 4U : 3U) : query ? (frp ? 5U : 4U) : 7U)) goto done;
    if (frp && (read || query) && !copy_id(root, "device_id", out->request.device_id)) goto done;
    if (!copy_id(root, "request_id", out->request.request_id)) goto done;
    if (status) { out->kind = EBASE_STATUS; error = NULL; goto done; }
    if (firmware_status) { out->kind = EBASE_FIRMWARE_STATUS; error = NULL; goto done; }
    if (business_status) { out->kind = EBASE_BUSINESS_STATUS; error = NULL; goto done; }
    if (query) {
        if (!allocate_payload(out, ESP_BASE_OTA_OPERATION_ID_BYTES, allocate)) {
            error = "resource_failure"; goto done;
        }
        const char *const keys[] = {"operation_id"};
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
        if (!exact_keys(parameters, keys, 1) || !copy_id(parameters, "operation_id", out->operation_id)) goto done;
        out->kind = EBASE_OTA_RESULT;
        error = NULL;
        goto done;
    }
    if (!write_identity(root, &out->request)) goto done;
    if (!strcmp(command->valuestring, "business.pause") ||
        !strcmp(command->valuestring, "business.resume")) {
        if (!exact_keys(cJSON_GetObjectItemCaseSensitive(root, "parameters"), NULL, 0)) goto done;
        out->kind = !strcmp(command->valuestring, "business.pause") ? EBASE_BUSINESS_PAUSE : EBASE_BUSINESS_RESUME;
        error = NULL; goto done;
    }
    if (!strcmp(command->valuestring, "ota.start")) {
        if (!allocate_payload(out, sizeof *out->ota, allocate)) {
            error = "resource_failure"; goto done;
        }
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
        const char *const keys[] = {"operation_id", "image_url", "sha256",
            "image_size_bytes", "target", "signature"};
        const char *const stream_keys[] = {"operation_id", "sha256", "image_size_bytes", "target", "signature"};
        if (!exact_keys(parameters, frp ? stream_keys : keys, frp ? 5U : 6U) ||
            !copy_id(parameters, "operation_id", out->ota->operation_id)) goto done;
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(parameters, "image_url");
        const cJSON *digest = cJSON_GetObjectItemCaseSensitive(parameters, "sha256");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(parameters, "image_size_bytes");
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(parameters, "target");
        const cJSON *signature = cJSON_GetObjectItemCaseSensitive(parameters, "signature");
        const char *const signature_keys[] = {"scheme"};
        const cJSON *scheme = cJSON_GetObjectItemCaseSensitive(signature, "scheme");
        if ((!frp && (!cJSON_IsString(url) || strlen(url->valuestring) > EOTA_URL_BYTES ||
            strncmp(url->valuestring, "https://", 8) != 0)) ||
            !cJSON_IsString(digest) || strlen(digest->valuestring) != 64 ||
            !cJSON_IsNumber(size) || !isfinite(size->valuedouble) || size->valuedouble < 1 ||
            size->valuedouble > UINT32_MAX || floor(size->valuedouble) != size->valuedouble ||
            !cJSON_IsString(target) || strcmp(target->valuestring, ESP_BASE_OTA_TARGET) ||
            !exact_keys(signature, signature_keys, 1) || !cJSON_IsString(scheme) ||
            strcmp(scheme->valuestring, ESP_BASE_OTA_SIGNATURE_SCHEME)) goto done;
        if (!digest32(digest, out->ota->sha256)) goto done;
        if (!frp) memcpy(out->ota->image_url, url->valuestring, strlen(url->valuestring) + 1);
        out->ota->inbound_stream = frp;
        out->ota->image_size_bytes = (uint32_t)size->valuedouble;
        out->kind = EBASE_OTA_START;
        error = NULL;
        goto done;
    }
    if (!strcmp(command->valuestring, "config.set")) {
        if (!allocate_payload(out, sizeof *out->config, allocate)) {
            error = "resource_failure"; goto done;
        }
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
        if (!cJSON_IsNumber(schema) || schema->valuedouble != 3) goto done;
        if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(config, "business"))) {
            error = "unsupported_configuration"; goto done;
        }
        out->config->revision = (uint32_t)revision->valuedouble;
        const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(config, "wifi");
        if (!cJSON_IsNull(wifi)) {
            const char *const wifi_keys[] = {"ssid", "password"};
            if (!exact_keys(wifi, wifi_keys, 2)) goto done;
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
            const cJSON *password = cJSON_GetObjectItemCaseSensitive(wifi, "password");
            if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) > 32 ||
                !cJSON_IsString(password) || strlen(password->valuestring) > 64) goto done;
            out->config->wifi.configured = true;
            memcpy(out->config->wifi.ssid, ssid->valuestring, strlen(ssid->valuestring));
            memcpy(out->config->wifi.password, password->valuestring, strlen(password->valuestring));
        }
        const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(config, "mqtt");
        if (!cJSON_IsNull(mqtt)) {
            const char *const mqtt_keys[] = {
                "hostname", "port", "username", "password", "ca_pem", "management_key_hex"
            };
            if (!exact_keys(mqtt, mqtt_keys, 6)) goto done;
            const cJSON *hostname = cJSON_GetObjectItemCaseSensitive(mqtt, "hostname");
            const cJSON *port = cJSON_GetObjectItemCaseSensitive(mqtt, "port");
            const cJSON *username = cJSON_GetObjectItemCaseSensitive(mqtt, "username");
            const cJSON *password = cJSON_GetObjectItemCaseSensitive(mqtt, "password");
            const cJSON *ca = cJSON_GetObjectItemCaseSensitive(mqtt, "ca_pem");
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(mqtt, "management_key_hex");
            if (!cJSON_IsString(hostname) || strlen(hostname->valuestring) > 253 ||
                !cJSON_IsNumber(port) || !isfinite(port->valuedouble) || port->valuedouble < 1 ||
                port->valuedouble > UINT16_MAX || floor(port->valuedouble) != port->valuedouble ||
                !cJSON_IsString(username) || strlen(username->valuestring) > 128 ||
                !cJSON_IsString(password) || strlen(password->valuestring) > 256 ||
                !cJSON_IsString(ca) || strlen(ca->valuestring) > 4096 ||
                !cJSON_IsString(key) || strlen(key->valuestring) != 64) goto done;
            out->config->mqtt.configured = true;
            out->config->mqtt.port = (uint16_t)port->valuedouble;
            memcpy(out->config->mqtt.hostname, hostname->valuestring, strlen(hostname->valuestring));
            memcpy(out->config->mqtt.username, username->valuestring, strlen(username->valuestring));
            memcpy(out->config->mqtt.password, password->valuestring, strlen(password->valuestring));
            memcpy(out->config->mqtt.ca_pem, ca->valuestring, strlen(ca->valuestring));
            for (size_t i = 0; i < EBASE_MQTT_KEY_BYTES; ++i) {
                const int hi = hex_digit(key->valuestring[2 * i]);
                const int lo = hex_digit(key->valuestring[2 * i + 1]);
                if (hi < 0 || lo < 0) goto done;
                out->config->mqtt.management_key[i] = (uint8_t)((hi << 4) | lo);
            }
        }
        const cJSON *frp = cJSON_GetObjectItemCaseSensitive(config, "frp");
        if (!cJSON_IsNull(frp)) {
            const char *const frp_keys[] = {"server_hostname", "server_port", "token", "ca_pem",
                                            "proxy_name", "remote_port", "local_port", "management_key_hex"};
            if (!exact_keys(frp, frp_keys, 8)) goto done;
            const cJSON *host = cJSON_GetObjectItemCaseSensitive(frp, "server_hostname");
            const cJSON *port = cJSON_GetObjectItemCaseSensitive(frp, "server_port");
            const cJSON *token = cJSON_GetObjectItemCaseSensitive(frp, "token");
            const cJSON *ca = cJSON_GetObjectItemCaseSensitive(frp, "ca_pem");
            const cJSON *proxy = cJSON_GetObjectItemCaseSensitive(frp, "proxy_name");
            const cJSON *remote = cJSON_GetObjectItemCaseSensitive(frp, "remote_port");
            const cJSON *local = cJSON_GetObjectItemCaseSensitive(frp, "local_port");
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(frp, "management_key_hex");
            if (!cJSON_IsString(host) || strlen(host->valuestring) > 253 ||
                !cJSON_IsNumber(port) || !isfinite(port->valuedouble) || port->valuedouble < 1 ||
                port->valuedouble > UINT16_MAX || floor(port->valuedouble) != port->valuedouble ||
                !cJSON_IsString(token) || strlen(token->valuestring) > EBASE_FRP_TOKEN_MAX_BYTES ||
                !cJSON_IsString(ca) || strlen(ca->valuestring) > EBASE_FRP_CA_MAX_BYTES ||
                !cJSON_IsString(proxy) || strlen(proxy->valuestring) > 128 ||
                !cJSON_IsNumber(remote) || !isfinite(remote->valuedouble) || remote->valuedouble < 1 ||
                remote->valuedouble > UINT16_MAX || floor(remote->valuedouble) != remote->valuedouble ||
                !cJSON_IsNumber(local) || !isfinite(local->valuedouble) || local->valuedouble < 1 ||
                local->valuedouble > UINT16_MAX || floor(local->valuedouble) != local->valuedouble ||
                !cJSON_IsString(key) || strlen(key->valuestring) != 64) goto done;
            out->config->frp.configured = true;
            out->config->frp.server_port = (uint16_t)port->valuedouble;
            out->config->frp.remote_port = (uint16_t)remote->valuedouble;
            out->config->frp.local_port = (uint16_t)local->valuedouble;
            memcpy(out->config->frp.server_hostname, host->valuestring, strlen(host->valuestring));
            memcpy(out->config->frp.token, token->valuestring, strlen(token->valuestring));
            memcpy(out->config->frp.ca_pem, ca->valuestring, strlen(ca->valuestring));
            memcpy(out->config->frp.proxy_name, proxy->valuestring, strlen(proxy->valuestring));
            for (size_t i = 0; i < EBASE_FRP_KEY_BYTES; ++i) {
                const int hi = hex_digit(key->valuestring[2 * i]);
                const int lo = hex_digit(key->valuestring[2 * i + 1]);
                if (hi < 0 || lo < 0) goto done;
                out->config->frp.management_key[i] = (uint8_t)((hi << 4) | lo);
            }
        }
        if (!ebase_config_valid(out->config)) goto done;
        out->kind = EBASE_CONFIG_SET;
        error = NULL;
        goto done;
    }
    if (strcmp(command->valuestring, "restart")) { error = "unsupported_command"; goto done; }
    if (!restart_parameters(root, &out->request)) goto done;
    out->kind = EBASE_RESTART;
    error = NULL;
done:
    if (error) release_payload(out);
    cJSON_Delete(root);
    return error;
}

const char *ebase_parse_command(const char *json, size_t length,
                                ebase_command_t *out, ebase_command_alloc_t allocate)
{
    return parse_command(json, length, out, allocate, false);
}

const char *ebase_parse_frp_command(const char *json, size_t length,
                                ebase_command_t *out, ebase_command_alloc_t allocate)
{
    return parse_command(json, length, out, allocate, true);
}

const char *ebase_parse_frp_status(const char *json, size_t length,
                                  ebase_request_t *out)
{
    if (!out) return "invalid_request";
    memset(out, 0, sizeof *out);
    if (!json || !length || length > 512 ||
        !valid_bytes((const unsigned char *)json, length)) return "invalid_request";
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return "invalid_request";
    while (end < json + length && (*end == ' ' || *end == '\t' ||
                                   *end == '\r' || *end == '\n')) ++end;
    const char *error = "invalid_request";
    const char *const keys[] = {"protocol_version", "device_id", "request_id", "command"};
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (end == json + length && exact_keys(root, keys, 4) &&
        cJSON_IsNumber(version) && version->valuedouble == 1 &&
        cJSON_IsString(command) && !strcmp(command->valuestring, "status") &&
        copy_id(root, "device_id", out->device_id) &&
        copy_id(root, "request_id", out->request_id)) {
        error = NULL;
    }
    if (error) memset(out, 0, sizeof *out);
    cJSON_Delete(root);
    return error;
}

const char *ebase_parse_frp_restart(const char *json, size_t length,
                                   ebase_request_t *out)
{
    if (!out) return "invalid_request";
    memset(out, 0, sizeof *out);
    if (!json || !length || length > 384 ||
        !valid_bytes((const unsigned char *)json, length)) return "invalid_request";
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return "invalid_request";
    while (end < json + length && (*end == ' ' || *end == '\t' ||
                                   *end == '\r' || *end == '\n')) ++end;
    const char *error = end == json + length && write_identity(root, out) &&
        restart_parameters(root, out) ? NULL : "invalid_request";
    if (error) memset(out, 0, sizeof *out);
    cJSON_Delete(root);
    return error;
}

void ebase_line_release(ebase_line_reader_t *r)
{
    if (!r) return;
    volatile unsigned char *bytes = (volatile unsigned char *)r->data;
    for (size_t i = 0; i < r->capacity; ++i) bytes[i] = 0;
    free(r->data);
    memset(r, 0, sizeof *r);
}

void ebase_line_feed(ebase_line_reader_t *r, const void *bytes, size_t length,
                     ebase_line_handler_t handler, void *context)
{
    const unsigned char *input = bytes;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = input[i];
        if (c == '\n') {
            if (r->discard) handler(NULL, 0, context);
            else if (r->length) {
                r->data[r->length] = '\0';
                handler(r->data, r->length, context);
            }
            ebase_line_release(r);
        } else if (!r->discard) {
            if (!c || r->length == EBASE_LINE_LIMIT) {
                ebase_line_release(r);
                r->discard = true;
                continue;
            }
            if (r->length + 1U >= r->capacity) {
                size_t capacity = r->capacity ? r->capacity * 2U : 64U;
                if (capacity > EBASE_LINE_LIMIT + 1U) capacity = EBASE_LINE_LIMIT + 1U;
                char *data = malloc(capacity);
                if (!data) {
                    ebase_line_release(r);
                    r->discard = true;
                    continue;
                }
                if (r->length) memcpy(data, r->data, r->length);
                size_t used = r->length;
                ebase_line_release(r);
                r->data = data;
                r->capacity = capacity;
                r->length = used;
            }
            r->data[r->length++] = (char)c;
        }
    }
}
