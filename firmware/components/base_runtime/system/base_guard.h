// SPDX-License-Identifier: Apache-2.0
// 业务守卫（安全模式的判定）：纯逻辑，不依赖 ESP-IDF。
// 同一个固件连续 BASE_GUARD_LIMIT 次异常重启，就不再拉起业务；正常重启（上电、远程重启、有线刷写）
// 或换了固件后重新计数，业务稳定运行一段时间后也清零。
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "base_protocol.h"

#define BASE_GUARD_LIMIT 3u
#define BASE_GUARD_STABLE_MS 300000u /* 业务连续运行这么久算稳定 */
#define BASE_GUARD_ERROR "crash_loop"

/* 持久记录（NVS blob），布局固定。 */
typedef struct {
    char fw[BASE_SHA256_HEX_BYTES]; /* 计数对应的固件 */
    uint8_t strikes;                /* 连续异常重启的次数，最大 BASE_GUARD_LIMIT */
} base_guard_t;

/* 启动时调用一次。loaded：guard 是从 NVS 读到的（否则当作空记录）。
 * abnormal：这次启动之前是异常复位（panic、各类看门狗），或运行时因业务看门狗超时重启了设备。
 * 返回是否拉起业务；*save 为真时调用方把 guard 写回 NVS。 */
bool base_guard_boot(base_guard_t *guard, bool loaded, const char *fw_sha256, bool abnormal, bool *save);
/* 业务稳定运行满 BASE_GUARD_STABLE_MS 后调用；返回是否需要写回。 */
bool base_guard_stable(base_guard_t *guard);
