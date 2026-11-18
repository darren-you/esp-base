// SPDX-License-Identifier: Apache-2.0
#include "base_guard.h"

#include <stdio.h>
#include <string.h>

bool base_guard_boot(base_guard_t *guard, bool loaded, const char *fw_sha256, bool abnormal, bool *save)
{
    if (!loaded) memset(guard, 0, sizeof *guard);
    const base_guard_t before = *guard;
    if (strncmp(guard->fw, fw_sha256, sizeof guard->fw) != 0) {
        /* 换了固件（升级、回滚、有线刷写）：重新计数。 */
        memset(guard, 0, sizeof *guard);
        snprintf(guard->fw, sizeof guard->fw, "%s", fw_sha256);
    }
    if (!abnormal) guard->strikes = 0;
    else if (guard->strikes < BASE_GUARD_LIMIT) ++guard->strikes;
    /* 没有记录且不用计数时不写 NVS：正常使用中不产生写入。 */
    *save = loaded ? memcmp(&before, guard, sizeof before) != 0 : guard->strikes != 0;
    return guard->strikes < BASE_GUARD_LIMIT;
}

bool base_guard_stable(base_guard_t *guard)
{
    if (guard->strikes == 0 || guard->strikes >= BASE_GUARD_LIMIT) return false;
    guard->strikes = 0;
    return true;
}
