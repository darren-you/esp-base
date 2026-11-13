// SPDX-License-Identifier: Apache-2.0
// 公网 OTA：声明式期望固件 + HTTPS 断点拉取 + 原生 A/B 回滚。
// 状态机（ota_logic.c）是纯逻辑，不依赖 ESP-IDF；下载任务（ota_worker.c）只用官方 esp_https_ota。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_protocol.h"

#define BASE_OTA_PERSIST_STEP_BYTES 65536u /* 偏移每 64 KiB 存一次 NVS */
#define BASE_OTA_MAX_ROLLBACKS 3u
#define BASE_OTA_RETRY_MIN_MS 5000u
#define BASE_OTA_RETRY_MAX_MS 300000u

typedef enum {
    BASE_OTA_IDLE,
    BASE_OTA_DOWNLOADING,
    BASE_OTA_VERIFYING,
    BASE_OTA_REBOOTING, /* 已切换启动槽；新固件连上 Broker 前一直保持 */
    BASE_OTA_SUCCEEDED,
    BASE_OTA_FAILED,
    BASE_OTA_REJECTED,
} base_ota_phase_t;

/* 唯一的持久记录（NVS blob），布局固定。 */
typedef struct {
    uint8_t version; /* BASE_OTA_RECORD_VERSION */
    uint8_t phase;
    uint8_t attempt;   /* 本任务已开始的下载次数 */
    uint8_t rollbacks; /* 本任务因自检失败回滚的次数 */
    uint32_t size;
    uint32_t offset; /* 已写入备用槽且已落盘的偏移 */
    char job[BASE_UUID_BYTES];
    char sha256[BASE_SHA256_HEX_BYTES];
    char error[BASE_ERROR_BYTES];
} base_ota_record_t;
#define BASE_OTA_RECORD_VERSION 1

typedef struct {
    char version[32];
    char sha256[BASE_SHA256_HEX_BYTES]; /* 运行槽内完整 signed bin 的摘要 */
    char target[48];
    char slot[17];
} base_fw_t;

typedef enum {
    BASE_OTA_RESULT_SWITCHED, /* 镜像校验通过，启动槽已切换 */
    BASE_OTA_RESULT_RETRY,    /* 网络类错误：退避后续传 */
    BASE_OTA_RESULT_REJECT,   /* 不可恢复：签名、摘要、target、容量、403／404 */
    BASE_OTA_RESULT_CANCELED,
} base_ota_result_t;

/* 每个输入返回一组动作，由调用方执行。 */
enum {
    BASE_OTA_ACT_PERSIST = 1 << 0,     /* 把 record 写入 NVS */
    BASE_OTA_ACT_PUBLISH = 1 << 1,     /* 发布 ota/state */
    BASE_OTA_ACT_START = 1 << 2,       /* 以 desired.url 和 record.offset 启动下载任务 */
    BASE_OTA_ACT_CANCEL = 1 << 3,      /* 要求下载任务中止 */
    BASE_OTA_ACT_RETRY_TIMER = 1 << 4, /* 启动重试定时器，时长见 base_ota_retry_delay_ms */
    BASE_OTA_ACT_STOP_TIMER = 1 << 5,
    BASE_OTA_ACT_REBOOT = 1 << 6,
};

typedef struct {
    base_ota_record_t record;
    base_ota_desired_t desired; /* 最近一次有效的期望固件，只在 RAM */
    bool have_desired;
    bool worker_running;
    bool confirmed;          /* 本次启动的固件已确认有效（可以开始新的 OTA） */
    bool retry_pending;      /* 重试定时器在跑 */
    uint8_t fail_streak;     /* 连续无进展的失败次数，决定退避时长 */
    uint32_t written;        /* 下载任务报告的实时偏移 */
    uint32_t published;      /* 上次发布状态时的偏移 */
    uint32_t attempt_start;  /* 本次下载开始时的偏移，用于判断是否有进展 */
    base_fw_t running;
    uint32_t slot_bytes;
} base_ota_t;

/* 启动时调用一次。stored 为 NULL 表示没有持久记录。 */
unsigned base_ota_boot(base_ota_t *ota, const base_ota_record_t *stored, const base_fw_t *running,
                       uint32_t slot_bytes, bool confirmed);
/* 本次启动的固件被确认有效（连上 Broker）。 */
unsigned base_ota_on_confirmed(base_ota_t *ota);
unsigned base_ota_on_desired(base_ota_t *ota, const base_ota_desired_t *desired);
/* 期望固件可识别任务但字段无效。 */
unsigned base_ota_on_invalid_desired(base_ota_t *ota, const char *job, const char *error);
/* 空载荷：取消。 */
unsigned base_ota_on_cancel(base_ota_t *ota);
unsigned base_ota_on_progress(base_ota_t *ota, uint32_t written);
unsigned base_ota_on_verifying(base_ota_t *ota);
unsigned base_ota_on_worker_done(base_ota_t *ota, base_ota_result_t result, const char *error);
unsigned base_ota_on_retry_timer(base_ota_t *ota);
/* 5 秒起、每次翻倍、上限 5 分钟，另加最多 20% 抖动。 */
uint32_t base_ota_retry_delay_ms(const base_ota_t *ota, uint32_t random);
const char *base_ota_phase_name(base_ota_phase_t phase);
/* 编码 ota/state；返回长度，缓冲不足返回 -1。 */
int base_ota_state_encode(const base_ota_t *ota, char *out, size_t size);

/* ---- 下载任务（仅设备）---- */
typedef enum {
    BASE_OTA_WORKER_PROGRESS,  /* value = 已写入偏移 */
    BASE_OTA_WORKER_VERIFYING,
    BASE_OTA_WORKER_DONE,      /* value = base_ota_result_t，error 为原因 */
} base_ota_worker_event_t;
typedef void (*base_ota_worker_cb_t)(base_ota_worker_event_t event, uint32_t value, const char *error);

/* 启动一次下载。同一时刻只允许一个；任务结束时必定回调一次 DONE。 */
bool base_ota_worker_start(const char *url, const char *sha256, uint32_t size, uint32_t offset,
                           base_ota_worker_cb_t callback);
void base_ota_worker_cancel(void);
/* 计算运行槽内完整 signed bin 的 SHA-256 并填写固件身份。 */
bool base_ota_running_firmware(base_fw_t *out, uint32_t *slot_bytes, bool *pending_verify);
