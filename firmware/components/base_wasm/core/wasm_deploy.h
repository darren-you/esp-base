// SPDX-License-Identifier: Apache-2.0
// 应用部署的状态机：声明式的期望应用，下载到备用槽，试运行，提交或回退。纯逻辑，不依赖 ESP-IDF。
//
// 设备上有两个应用槽。新应用总是下载到没在用的那个槽，校验通过后停旧启新；新应用稳定运行 WASM_TRIAL_MS
// 才提交，期间启动失败、出故障或把设备拖到重启，都回到原来的槽并把任务记为失败。提交之后的故障不回退，
// 由宿主按退避重启。与固件 OTA 一样，Server 只发布「应当运行哪个应用」，设备每次上线自行收敛。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_package.h"

#define WASM_SLOTS 2
#define WASM_SLOT_NONE 0xFFu
#define WASM_TRIAL_MS 30000u /* 新应用稳定运行这么久才提交 */
#define WASM_TRIAL_BOOTS 2u  /* 试运行期间设备第二次重启：认定是新应用拖垮了设备，回退 */
#define WASM_CRASH_LIMIT 2u  /* 已提交的应用运行时设备连续异常重启这么多次：停用应用，宿主照常在线等新的部署 */
#define WASM_STABLE_MS 300000u /* 应用连续运行这么久算稳定：异常重启的计数清零 */
#define WASM_DEPLOY_RETRY_MIN_MS 5000u
#define WASM_DEPLOY_RETRY_MAX_MS 300000u
#define WASM_UUID_BYTES 37
#define WASM_SHA256_HEX_BYTES 65
#define WASM_ERROR_BYTES 24
#define WASM_URL_BYTES 512
#define WASM_DEPLOY_STATE_BYTES 512

typedef enum {
    WASM_DEPLOY_IDLE,
    WASM_DEPLOY_DOWNLOADING,
    WASM_DEPLOY_VERIFYING,
    WASM_DEPLOY_STARTING, /* 新应用在试运行，尚未提交 */
    WASM_DEPLOY_SUCCEEDED,
    WASM_DEPLOY_FAILED,
    WASM_DEPLOY_REJECTED,
} wasm_deploy_phase_t;

/* 唯一的持久记录（NVS blob），布局固定。 */
typedef struct {
    uint8_t version; /* WASM_DEPLOY_RECORD_VERSION */
    uint8_t active;  /* 已提交的槽；WASM_SLOT_NONE：没有应用 */
    uint8_t trial;   /* 试运行中的槽；WASM_SLOT_NONE：没有 */
    uint8_t trial_boots;
    uint8_t phase; /* 最近一次任务的阶段 */
    uint8_t attempt;
    uint8_t crashes; /* 已提交的应用运行时，设备连续异常重启的次数 */
    char job[WASM_UUID_BYTES];
    char sha256[WASM_SHA256_HEX_BYTES]; /* 任务期望的应用包 */
    char error[WASM_ERROR_BYTES];
} wasm_deploy_record_t;
#define WASM_DEPLOY_RECORD_VERSION 1

/* 期望应用（app/desired）。 */
typedef struct {
    char job[WASM_UUID_BYTES];
    char sha256[WASM_SHA256_HEX_BYTES]; /* 完整应用包（含签名扇区）的 SHA-256 */
    uint32_t size;
    char url[WASM_URL_BYTES];
} wasm_desired_t;

/* 一个槽里的应用包；平台在启动时和每次安装后读槽、验签得到。 */
typedef struct {
    bool valid;
    char sha256[WASM_SHA256_HEX_BYTES];
    char name[WASM_PACKAGE_NAME_BYTES];
    char version[WASM_PACKAGE_VERSION_BYTES];
    uint32_t abi;
} wasm_slot_t;

typedef enum {
    WASM_WORKER_INSTALLED, /* 下载并校验通过：包已在目标槽里 */
    WASM_WORKER_RETRY,     /* 网络类错误或停滞：退避后重试 */
    WASM_WORKER_REJECT,    /* 不可恢复：大小、摘要、签名、清单、接口版本、芯片 */
    WASM_WORKER_CANCELED,
} wasm_worker_result_t;

/* 每个输入返回一组动作，由调用方执行。 */
enum {
    WASM_ACT_PERSIST = 1 << 0,     /* 把 record 写入 NVS */
    WASM_ACT_PUBLISH = 1 << 1,     /* 发布 app/state */
    WASM_ACT_DOWNLOAD = 1 << 2,    /* 把 desired.url 下载到 wasm_deploy_target() 那个槽 */
    WASM_ACT_CANCEL = 1 << 3,      /* 要求下载任务中止 */
    WASM_ACT_RETRY_TIMER = 1 << 4, /* 启动重试定时器，时长见 wasm_deploy_retry_delay_ms */
    WASM_ACT_STOP_TIMER = 1 << 5,
    WASM_ACT_RUN = 1 << 6,         /* 停掉当前应用，启动 running 那个槽里的应用（WASM_SLOT_NONE：只停） */
    WASM_ACT_TRIAL_TIMER = 1 << 7, /* 启动试运行定时器（WASM_TRIAL_MS） */
    WASM_ACT_STOP_TRIAL = 1 << 8,
};

typedef struct {
    wasm_deploy_record_t record;
    wasm_desired_t desired; /* 最近一次有效的期望应用，只在 RAM */
    bool have_desired;
    bool deferred;       /* desired 是在别的任务还没了结时到的，了结后再处理 */
    bool worker_running; /* 下载任务在跑 */
    bool canceling;      /* 已要求下载任务中止，等它结束 */
    bool retry_pending;  /* 重试定时器在跑 */
    uint8_t fail_streak; /* 连续失败的下载次数，决定退避时长 */
    uint8_t running;     /* 应当运行的槽；WASM_SLOT_NONE：没有应用 */
    bool disabled;       /* 已提交的应用连续把设备拖到异常重启，这次启动不运行它 */
    wasm_slot_t slots[WASM_SLOTS];
} wasm_deploy_t;

/* 启动时调用一次。stored 为 NULL 表示没有持久记录；slots 是两个槽当前的内容；
 * abnormal 表示这次启动之前是异常复位（panic、各类看门狗）。 */
unsigned wasm_deploy_boot(wasm_deploy_t *deploy, const wasm_deploy_record_t *stored, const wasm_slot_t slots[WASM_SLOTS],
                          bool abnormal);
/* 应用连续运行满 WASM_STABLE_MS。 */
unsigned wasm_deploy_on_stable(wasm_deploy_t *deploy);
unsigned wasm_deploy_on_desired(wasm_deploy_t *deploy, const wasm_desired_t *desired);
/* 期望应用可识别任务但字段无效。 */
unsigned wasm_deploy_on_invalid_desired(wasm_deploy_t *deploy, const char *job, const char *error);
/* 空载荷：取消还没装好的任务。 */
unsigned wasm_deploy_on_cancel(wasm_deploy_t *deploy);
unsigned wasm_deploy_on_verifying(wasm_deploy_t *deploy);
/* 下载任务结束。INSTALLED 时 installed 是目标槽的新内容。 */
unsigned wasm_deploy_on_worker_done(wasm_deploy_t *deploy, wasm_worker_result_t result, const char *error,
                                    const wasm_slot_t *installed);
/* 平台执行完 WASM_ACT_RUN：应用启动成功与否。 */
unsigned wasm_deploy_on_started(wasm_deploy_t *deploy, bool ok, const char *error);
/* 运行中的应用出了故障（陷阱、超预算）。只在试运行期间有后果。 */
unsigned wasm_deploy_on_fault(wasm_deploy_t *deploy, const char *error);
unsigned wasm_deploy_on_trial_timer(wasm_deploy_t *deploy);
unsigned wasm_deploy_on_retry_timer(wasm_deploy_t *deploy);

/* 下载的目标槽：没在用的那个。 */
uint8_t wasm_deploy_target(const wasm_deploy_t *deploy);
/* 5 秒起、每次翻倍、上限 5 分钟，另加最多 20% 抖动。 */
uint32_t wasm_deploy_retry_delay_ms(const wasm_deploy_t *deploy, uint32_t random);
const char *wasm_deploy_phase_name(wasm_deploy_phase_t phase);
/* 解码 app/desired。成功返回 NULL；失败返回原因（invalid_desired、invalid_url、package_too_large），
 * 这时 out->job 里如果有任务号，调用方用 wasm_deploy_on_invalid_desired 上报，否则丢弃。 */
const char *wasm_desired_decode(const char *payload, size_t length, uint32_t slot_bytes, wasm_desired_t *out);
/* 应用此刻的运行情况，由平台给出。 */
typedef struct {
    const char *state; /* running、stopped（没有应用）、crashed（出了故障，等着重启）、disabled（连续拖垮设备，已停用） */
    const char *fault; /* 最近一次故障的原因；NULL：没有 */
    uint32_t restarts; /* 本次启动以来因故障重启应用的次数 */
    uint32_t pool_used, pool_peak, linear_bytes;
} wasm_app_status_t;

/* 编码 app/state；返回长度，缓冲不足返回 -1。 */
int wasm_deploy_state_encode(const wasm_deploy_t *deploy, const wasm_app_status_t *app, char *out, size_t size);
