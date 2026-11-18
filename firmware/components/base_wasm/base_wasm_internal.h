// SPDX-License-Identifier: Apache-2.0
// WASM 宿主设备端各文件之间的接口：事件、平台服务（wasm_platform.c）和应用槽（wasm_store.c）。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_deploy.h"
#include "wasm_package.h"

/* 应用任务的事件。定时器回调、业务连接的回调、下载任务和系统层都只向应用任务的队列投递事件。 */
typedef enum {
    EV_TIMER,          /* 应用的定时器到期 */
    EV_LINK_UP,        /* 业务连接连上／断开／收到消息 */
    EV_LINK_DOWN,
    EV_LINK_MESSAGE,
    EV_SYSTEM_CONNECT, /* 系统通知：可以建立业务连接了 */
    EV_RESTART,        /* 出过故障的应用该重新启动了 */
    EV_DESIRED,        /* 系统转来的 app/desired */
    EV_FETCH_VERIFYING,
    EV_FETCH_DONE,
    EV_TRIAL_TIMER,
    EV_RETRY_TIMER,
    EV_STABLE_TIMER,
} base_wasm_event_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t timer;                         /* EV_TIMER：定时器编号 */
    uint16_t topic_length, payload_length; /* EV_LINK_MESSAGE；EV_DESIRED 只用 payload_length */
    uint32_t epoch;                        /* EV_TIMER：设置时的代，对不上说明它后来被重设或取消了 */
    void *client;                          /* EV_LINK_*：产生事件的连接，与当前连接不同的丢弃 */
    char *data;                            /* EV_LINK_MESSAGE：主题紧跟载荷；EV_DESIRED：载荷。处理后释放 */
} base_wasm_event_t;

/* 投递事件；队列满时丢弃并计数（附带的数据一并释放）。可以在任意任务和定时器回调里调用。 */
void base_wasm_post(const base_wasm_event_t *event);
/* 建一个记在业务名下的任务；系统层没有启动时（测试固件）退回普通任务。 */
bool base_wasm_task(const char *name, void (*entry)(void *), uint32_t stack_bytes, uint32_t watchdog_ms);

/* ---- 平台服务：定时器与业务连接（wasm_platform.c）。除特别说明外只在应用任务里调用。 ---- */
bool wasm_platform_init(const char *device_id);
/* 系统通知可以建立业务连接：应用要求过连接就按当前的业务 MQTT 配置（重新）建立。 */
void wasm_platform_connect_allowed(void);
/* 连接事件是否来自当前连接；连上时顺带重新订阅。 */
bool wasm_platform_link_up(void *client);
bool wasm_platform_link_down(void *client);
bool wasm_platform_link_current(void *client);
/* 消息事件处理完：释放数据，归还排队额度。 */
void wasm_platform_message_done(const base_wasm_event_t *event);
bool wasm_platform_timer_current(uint8_t id, uint32_t epoch);
/* 把应用声明的引脚置到安全状态并记下来：开机时、应用启动前调用；应用停止时平台按记下的这一组再置一次。
 * 上一组里有、这一组里没有的引脚还给系统（高阻输入）。 */
void wasm_platform_pins(const wasm_pin_t *pins, unsigned count);
void wasm_platform_count_drop(void);
unsigned wasm_platform_dropped(void);

/* ---- 应用槽、部署记录与下载（wasm_store.c） ---- */
#define WASM_SLOT_BYTES 32768u
#define WASM_MEMORY_MAX_BYTES (40u * 1024u) /* 清单里 memory 的上限：两种芯片相同 */

/* 登记 Flash 末尾的两个应用槽。失败说明分区表占用了那块区域。在系统的控制任务里调用。 */
bool wasm_store_init(void);
/* 读一个槽：包头与清单、签名、摘要、这个固件能否运行。成功时 slot->valid 为真；失败时 *error 是原因。
 * 验签用官方 esp_secure_boot_verify_signature，它不可重入：只在控制任务（启动时）和下载任务里调用，
 * 而下载任务不与固件下载同时运行。 */
bool wasm_store_inspect(uint8_t index, wasm_slot_t *slot, wasm_package_t *package, const char **error);
bool wasm_store_read(uint8_t index, uint32_t offset, uint8_t *out, uint32_t bytes);
bool wasm_store_load_record(wasm_deploy_record_t *out);
void wasm_store_save_record(const wasm_deploy_record_t *record);

typedef struct {
    wasm_worker_result_t result;
    char error[WASM_ERROR_BYTES];
    wasm_slot_t installed;
    wasm_package_t package;
} wasm_fetch_result_t;

/* 把期望应用下载到指定的槽并校验。结束时投递 EV_FETCH_DONE，结果用 wasm_fetch_result 读取。同一时刻只有一个。 */
bool wasm_fetch_start(const wasm_desired_t *desired, uint8_t slot);
void wasm_fetch_cancel(void);
const wasm_fetch_result_t *wasm_fetch_result(void);
