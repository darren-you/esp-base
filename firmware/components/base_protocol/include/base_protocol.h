// SPDX-License-Identifier: Apache-2.0
// 设备—云协议的纯逻辑：主题、命令解码、应答与去重、原生业务。不依赖 ESP-IDF，可在 host 上测试。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BASE_UUID_BYTES 37
#define BASE_SHA256_HEX_BYTES 65
#define BASE_TOPIC_BYTES 96
#define BASE_URL_BYTES 512
#define BASE_ERROR_BYTES 24
#define BASE_RSP_BYTES 224
#define BASE_CMD_NAME_BYTES 16
#define BASE_TOPIC_PREFIX "esp-base"

typedef enum {
    BASE_TOPIC_STATUS,      /* 设备 → Server，retained 状态文档；遗嘱也发到这里 */
    BASE_TOPIC_RSP,         /* 设备 → Server，命令应答 */
    BASE_TOPIC_CMD_FILTER,  /* Server → 设备，订阅过滤器 cmd/+ */
    BASE_TOPIC_OTA_DESIRED, /* Server → 设备，retained 期望固件 */
    BASE_TOPIC_OTA_STATE,   /* 设备 → Server，retained OTA 状态 */
} base_topic_t;

bool base_is_uuid(const char *text);
bool base_is_sha256_hex(const char *text);
bool base_topic(char out[BASE_TOPIC_BYTES], const char *device_id, base_topic_t topic);
/* topic 是本设备的 cmd/<name> 时写出 name 并返回 true。topic 不要求以 NUL 结尾。 */
bool base_topic_command(const char *topic, size_t topic_len, const char *device_id,
                        char name[BASE_CMD_NAME_BYTES]);

typedef enum { BASE_CMD_STATUS, BASE_CMD_RESTART, BASE_CMD_CONFIG, BASE_CMD_BUSINESS } base_cmd_kind_t;

typedef struct {
    bool has_wifi;
    char wifi_ssid[33];
    char wifi_password[65];
    bool has_mqtt;
    char mqtt_uri[128];
    char mqtt_username[65];
    char mqtt_password[129];
} base_config_update_t;

typedef struct {
    char request_id[BASE_UUID_BYTES];
    base_cmd_kind_t kind;
    base_config_update_t config; /* kind == BASE_CMD_CONFIG */
    char business_op[8];         /* kind == BASE_CMD_BUSINESS：add、pause、resume */
    uint32_t business_bytes;     /* add 时 data 字符串的字节数 */
} base_cmd_t;

/* 解码命令。返回 false 且 request_id 为空：载荷不可应答，丢弃。
 * 返回 false 且 request_id 非空：以 *error 应答。返回 true：执行。 */
bool base_cmd_decode(const char *name, const char *payload, size_t length,
                     base_cmd_t *out, const char **error);

/* 应答：{"request_id":…,"cmd":…,"ok":true|false,"error":null|"…","result":null|{…}}。
 * 返回写入长度；缓冲不足返回 -1。result_json 必须是合法 JSON 值或 NULL。 */
int base_rsp_encode(char out[BASE_RSP_BYTES], const char *request_id, const char *cmd,
                    const char *error, const char *result_json);

/* QoS 1 会重投：记住最近若干条应答，重复的 request_id 只重发原应答，不重复执行。 */
#define BASE_DEDUPE_SLOTS 8
typedef struct {
    struct {
        char request_id[BASE_UUID_BYTES];
        char response[BASE_RSP_BYTES];
    } entries[BASE_DEDUPE_SLOTS];
    unsigned next;
} base_dedupe_t;
const char *base_dedupe_find(const base_dedupe_t *dedupe, const char *request_id);
void base_dedupe_store(base_dedupe_t *dedupe, const char *request_id, const char *response);

/* 原生业务：消息字节计数，可暂停与恢复。 */
typedef struct {
    bool paused;
    uint32_t bytes;
} base_business_t;
/* 成功返回 NULL，否则返回错误码字符串。 */
const char *base_business_apply(base_business_t *business, const char *op, uint32_t data_bytes);
int base_business_encode(const base_business_t *business, char *out, size_t size);

/* OTA 期望固件（ota/desired）。 */
typedef struct {
    char job[BASE_UUID_BYTES];
    char version[32];
    char target[32];
    uint32_t size;
    char sha256[BASE_SHA256_HEX_BYTES];
    char url[BASE_URL_BYTES];
} base_ota_desired_t;

typedef enum {
    BASE_DESIRED_OK,
    BASE_DESIRED_EMPTY,   /* 空载荷：取消 */
    BASE_DESIRED_INVALID, /* 无法识别任务，丢弃 */
    BASE_DESIRED_REJECT,  /* 任务可识别但字段无效：out->job 与 *error 有效 */
} base_desired_result_t;
base_desired_result_t base_ota_desired_decode(const char *payload, size_t length,
                                              base_ota_desired_t *out, const char **error);
