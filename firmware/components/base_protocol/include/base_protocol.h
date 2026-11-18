// SPDX-License-Identifier: Apache-2.0
// 设备—云协议的纯逻辑：主题、命令解码、应答与去重。不依赖 ESP-IDF，可在 host 上测试。
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
#define BASE_NAME_BYTES 33 /* 设备名称：1–32 字节 UTF-8 */
#define BASE_URI_BYTES 128
#define BASE_TOPIC_PREFIX "esp-base"
/* 系统任务（控制、Profile 采样、OTA 下载）的优先级，与 esp-mqtt 任务的默认值相同；业务任务都低于它。 */
#define BASE_SYSTEM_TASK_PRIORITY 5

typedef enum {
    BASE_TOPIC_STATUS,      /* 设备 → Server，retained 状态文档；遗嘱也发到这里 */
    BASE_TOPIC_RSP,         /* 设备 → Server，命令应答 */
    BASE_TOPIC_CMD_FILTER,  /* Server → 设备，订阅过滤器 cmd/+ */
    BASE_TOPIC_OTA_DESIRED, /* Server → 设备，retained 期望固件 */
    BASE_TOPIC_OTA_STATE,   /* 设备 → Server，retained OTA 状态 */
    BASE_TOPIC_APP_DESIRED, /* Server → 设备，retained 期望应用 */
    BASE_TOPIC_APP_STATE,   /* 设备 → Server，retained 应用状态 */
    BASE_TOPIC_COUNT,
} base_topic_t;

bool base_is_uuid(const char *text);
/* 用 16 个随机字节写出一个版本 4 的小写 UUID。 */
void base_uuid_format(char out[BASE_UUID_BYTES], const uint8_t random[16]);
bool base_is_sha256_hex(const char *text);
bool base_topic(char out[BASE_TOPIC_BYTES], const char *device_id, base_topic_t topic);
/* topic 是本设备的 cmd/<name> 时写出 name 并返回 true。topic 不要求以 NUL 结尾。 */
bool base_topic_command(const char *topic, size_t topic_len, const char *device_id,
                        char name[BASE_CMD_NAME_BYTES]);

typedef enum { BASE_CMD_STATUS, BASE_CMD_RESTART, BASE_CMD_CONFIG } base_cmd_kind_t;

/* 一个 MQTT Broker 的连接信息。 */
typedef struct {
    char uri[BASE_URI_BYTES];
    char username[65];
    char password[129];
} base_mqtt_t;

/* config 命令改 Wi-Fi、业务 MQTT、设备名称和所属 Group。连本协议 Broker 的系统 MQTT 只由出厂资料写入：
 * 远程改错或改到别的 Broker，Server 就再也管不到这台设备。 */
typedef struct {
    bool has_wifi;
    char wifi_ssid[33];
    char wifi_password[65];
    bool has_business_mqtt;
    base_mqtt_t business_mqtt;
    bool has_name;
    char name[BASE_NAME_BYTES];
    bool has_group;
    char group[BASE_UUID_BYTES]; /* 空串：移出 Group */
} base_config_update_t;

typedef struct {
    char request_id[BASE_UUID_BYTES];
    base_cmd_kind_t kind;
    base_config_update_t config; /* kind == BASE_CMD_CONFIG */
} base_cmd_t;

/* 解码命令。返回 false 且 request_id 为空：载荷不可应答，丢弃。
 * 返回 false 且 request_id 非空：以 *error 应答。返回 true：执行。 */
bool base_cmd_decode(const char *name, const char *payload, size_t length,
                     base_cmd_t *out, const char **error);

/* 应答：{"request_id":…,"cmd":…,"ok":true|false,"error":null|"…","result":null}。
 * result 留给有返回值的命令，现有命令都没有。返回写入长度；缓冲不足返回 -1。 */
int base_rsp_encode(char out[BASE_RSP_BYTES], const char *request_id, const char *cmd, const char *error);

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

/* 运行固件的身份。 */
typedef struct {
    char version[32];
    char sha256[BASE_SHA256_HEX_BYTES]; /* 运行槽内完整 signed bin 的摘要 */
    char target[48];
    char slot[17];
} base_fw_t;

/* 芯片功能位，状态文档里写成字符串数组。 */
enum {
    BASE_CHIP_WIFI = 1 << 0,
    BASE_CHIP_BLE = 1 << 1,
    BASE_CHIP_BT = 1 << 2,
    BASE_CHIP_IEEE802154 = 1 << 3,
    BASE_CHIP_EMB_FLASH = 1 << 4,
    BASE_CHIP_EMB_PSRAM = 1 << 5,
};

#define BASE_TASK_NAME_BYTES 24
#define BASE_STACK_SLOTS 24
#define BASE_IPV6_SLOTS 3
#define BASE_IP_TEXT_BYTES 46
#define BASE_BUSINESS_NAME_BYTES 24 /* 业务名：1–23 字节的小写字母、数字和下划线 */
#define BASE_LINK_SLOTS 3           /* 业务经运行时打开的连接数上限 */
#define BASE_LINK_NAME_BYTES 16
#define BASE_BUSINESS_TASK_SLOTS 8

/* 一个任务栈的历史最小余量。 */
typedef struct {
    char name[BASE_TASK_NAME_BYTES];
    uint32_t bytes;
} base_task_stack_t;
/* 往列表里加一个任务：名称里字母、数字以外的字符换成下划线；与已有任务同名时加序号（mqtt_task、mqtt_task_2），
 * 避免状态文档里出现重复的键。列表满了返回 false。 */
bool base_task_stack_add(base_task_stack_t *list, uint8_t *count, unsigned slots, const char *task_name, uint32_t bytes);

/* Profile 里业务层的部分：由业务运行时填写，系统层只负责上报。 */
typedef struct {
    bool present;                          /* 固件带业务层；为假时状态文档里 business 为 null */
    char name[BASE_BUSINESS_NAME_BYTES];
    bool running;                          /* 为假：业务已停用（安全模式），没有调用任何业务代码 */
    char error[BASE_ERROR_BYTES];          /* 停用原因，例如 crash_loop；空串：没有 */
    char mqtt_uri[BASE_URI_BYTES];         /* 经 config 命令下发的业务 MQTT 地址；空串：没有配置 */
    struct {
        char name[BASE_LINK_NAME_BYTES];
        char uri[BASE_URI_BYTES];          /* 不含账户、密码和查询串 */
        bool connected;
    } links[BASE_LINK_SLOTS];
    uint8_t link_count;
    base_task_stack_t tasks[BASE_BUSINESS_TASK_SLOTS];
    uint8_t task_count;
} base_business_profile_t;

/* 设备 Profile：设备能报告的全部信息。由 base_profile 组件采集与持有，这里只定义结构和编码。
 * 顶层字段都属于系统；业务层的信息集中在 business 里。 */
typedef struct {
    /* 身份：启动时确定。 */
    char device_id[BASE_UUID_BYTES];
    char boot_id[BASE_UUID_BYTES];
    char reset[24];
    base_fw_t fw;
    /* 芯片：启动时采集一次。 */
    char chip_model[16];    /* IDF target，例如 esp32c3 */
    char chip_revision[12]; /* 例如 v0.4 */
    uint8_t cores;
    uint16_t cpu_mhz;
    uint32_t chip_features; /* BASE_CHIP_* */
    uint32_t flash_bytes;
    uint32_t psram_bytes;
    char idf[32];
    char mac[18]; /* Wi-Fi STA MAC，aa:bb:cc:dd:ee:ff */
    /* 设置：由控制任务写入。name 为空时上报默认名称。 */
    char name[BASE_NAME_BYTES];
    char group[BASE_UUID_BYTES]; /* 空串：未归属 Group */
    char mqtt_uri[BASE_URI_BYTES]; /* 系统 MQTT */
    uint32_t config_revision;
    char config_error[BASE_ERROR_BYTES]; /* 空串：没有失败 */
    uint32_t wakeups;
    /* 运行采样。 */
    uint32_t uptime_s;
    uint32_t heap_free, heap_min, heap_largest;
    base_task_stack_t stack[BASE_STACK_SLOTS]; /* 系统任务；业务任务在 business.tasks */
    uint8_t stack_count;
    int8_t rssi;
    char ssid[33];
    char bssid[18];
    uint8_t channel;
    char hostname[33];
    bool has_ipv4;
    char ipv4[16], netmask[16], gateway[16];
    char ipv6[BASE_IPV6_SLOTS][BASE_IP_TEXT_BYTES];
    uint8_t ipv6_count;
    /* 业务层：采样时由业务运行时填写。 */
    base_business_profile_t business;
} base_profile_t;

/* 默认名称：芯片显示名加 MAC 后 3 字节，例如 ESP32-C3-4A1B2C。mac 为 aa:bb:cc:dd:ee:ff。 */
void base_default_name(char out[BASE_NAME_BYTES], const char *chip_model, const char *mac);
/* 设备名称是否合法：1–32 字节，无控制字符，首尾不是空格。 */
bool base_is_device_name(const char *name);
/* 业务名、连接名这类标识是否合法：1 到 size-1 字节的小写字母、数字和下划线。 */
bool base_is_identifier(const char *text, size_t size);
/* 可以上报的地址：去掉账户、密码（userinfo）、查询串和片段，只留 scheme://host[:port][/path]。
 * 放不下或不是 scheme://… 时写出空串并返回 false。 */
bool base_public_uri(char out[BASE_URI_BYTES], const char *uri);
/* 状态文档。返回写入长度；缓冲不足返回 -1。 */
int base_status_encode(const base_profile_t *profile, char *out, size_t size);
/* 遗嘱：{"online":false,…}，只带 device_id、boot_id、name、group 和 fw。 */
int base_will_encode(const base_profile_t *profile, char *out, size_t size);
