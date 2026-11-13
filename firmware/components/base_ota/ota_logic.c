// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <string.h>

#include "base_ota.h"

static void set_error(base_ota_record_t *record, const char *error)
{
    memset(record->error, 0, sizeof record->error);
    if (error) snprintf(record->error, sizeof record->error, "%s", error);
}

static bool terminal(uint8_t phase)
{
    return phase == BASE_OTA_SUCCEEDED || phase == BASE_OTA_FAILED || phase == BASE_OTA_REJECTED;
}

static bool record_valid(const base_ota_record_t *record)
{
    return record->version == BASE_OTA_RECORD_VERSION && record->phase <= BASE_OTA_REJECTED &&
           memchr(record->job, 0, sizeof record->job) && memchr(record->sha256, 0, sizeof record->sha256) &&
           memchr(record->error, 0, sizeof record->error) &&
           (record->job[0] == '\0' || base_is_uuid(record->job)) && record->offset <= record->size;
}

static void finish(base_ota_t *ota, base_ota_phase_t phase, const char *error)
{
    ota->record.phase = (uint8_t)phase;
    set_error(&ota->record, error);
    ota->fail_streak = 0;
}

static void clear(base_ota_t *ota)
{
    memset(&ota->record, 0, sizeof ota->record);
    ota->record.version = BASE_OTA_RECORD_VERSION;
    ota->written = ota->published = 0;
    ota->fail_streak = 0;
}

/* 把 record 换成新任务，从偏移 0 开始。 */
static void adopt(base_ota_t *ota, const base_ota_desired_t *desired)
{
    memset(&ota->record, 0, sizeof ota->record);
    ota->record.version = BASE_OTA_RECORD_VERSION;
    ota->record.phase = BASE_OTA_DOWNLOADING;
    ota->record.size = desired->size;
    memcpy(ota->record.job, desired->job, sizeof ota->record.job);
    memcpy(ota->record.sha256, desired->sha256, sizeof ota->record.sha256);
    ota->written = ota->published = 0;
    ota->fail_streak = 0;
}

static unsigned start_download(base_ota_t *ota)
{
    unsigned actions = BASE_OTA_ACT_START | BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    if (ota->retry_pending) {
        ota->retry_pending = false;
        actions |= BASE_OTA_ACT_STOP_TIMER;
    }
    if (ota->record.attempt < UINT8_MAX) ++ota->record.attempt;
    ota->record.phase = BASE_OTA_DOWNLOADING;
    ota->written = ota->published = ota->attempt_start = ota->record.offset;
    ota->worker_running = true;
    return actions;
}

/* 按当前 record 与 desired 决定下一步；下载任务在跑时不做决定。 */
static unsigned reconcile(base_ota_t *ota)
{
    if (!ota->have_desired || ota->worker_running) return 0;
    const base_ota_desired_t *desired = &ota->desired;
    const bool same_job = !strcmp(ota->record.job, desired->job);

    /* 运行中的固件就是期望固件：成功，不下载。 */
    if (!strcmp(desired->sha256, ota->running.sha256)) {
        if (same_job && ota->record.phase == BASE_OTA_REBOOTING && !ota->confirmed) return 0;
        if (same_job && ota->record.phase == BASE_OTA_SUCCEEDED) return BASE_OTA_ACT_PUBLISH;
        adopt(ota, desired);
        ota->record.offset = desired->size;
        ota->written = desired->size;
        finish(ota, BASE_OTA_SUCCEEDED, NULL);
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    }
    if (same_job) {
        /* 同一任务的终态失败只回报，不重试；重试由 Server 创建新任务。 */
        if (ota->record.phase == BASE_OTA_FAILED || ota->record.phase == BASE_OTA_REJECTED)
            return BASE_OTA_ACT_PUBLISH;
        if (ota->record.phase == BASE_OTA_REBOOTING) return 0;
        if (ota->record.phase == BASE_OTA_DOWNLOADING || ota->record.phase == BASE_OTA_VERIFYING) {
            if (!ota->confirmed) return 0;
            /* 同一任务换了制品：从头开始。 */
            if (strcmp(ota->record.sha256, desired->sha256) || ota->record.size != desired->size) {
                const uint8_t attempt = ota->record.attempt, rollbacks = ota->record.rollbacks;
                adopt(ota, desired);
                ota->record.attempt = attempt;
                ota->record.rollbacks = rollbacks;
            }
            return start_download(ota);
        }
        /* 曾成功但现在运行的不是它（例如有线刷过别的固件）：按新任务处理。 */
    }
    if (ota->record.phase == BASE_OTA_REBOOTING && !ota->confirmed) return 0;
    adopt(ota, desired);
    if (strcmp(desired->target, ota->running.target)) {
        finish(ota, BASE_OTA_REJECTED, "target_mismatch");
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    }
    if (desired->size > ota->slot_bytes) {
        finish(ota, BASE_OTA_REJECTED, "image_too_large");
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    }
    if (!ota->confirmed) return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    return start_download(ota);
}

unsigned base_ota_boot(base_ota_t *ota, const base_ota_record_t *stored, const base_fw_t *running,
                       uint32_t slot_bytes, bool confirmed)
{
    memset(ota, 0, sizeof *ota);
    ota->running = *running;
    ota->slot_bytes = slot_bytes;
    ota->confirmed = confirmed;
    ota->record.version = BASE_OTA_RECORD_VERSION;
    if (!stored || !record_valid(stored)) return 0;
    ota->record = *stored;
    ota->written = ota->published = ota->record.offset;
    if (ota->record.phase == BASE_OTA_VERIFYING) {
        /* 校验中断电：回到下载，由剩余字节重新走完校验。 */
        ota->record.phase = BASE_OTA_DOWNLOADING;
        return BASE_OTA_ACT_PERSIST;
    }
    if (ota->record.phase != BASE_OTA_REBOOTING) return 0;
    if (!strcmp(ota->record.sha256, running->sha256)) {
        /* 新固件已启动。连上 Broker 之前不落盘成功，回滚后的旧固件才能数到这次失败。 */
        return confirmed ? base_ota_on_confirmed(ota) : 0;
    }
    /* 切换过启动槽，但运行的不是它：自检失败后被 bootloader 回滚。 */
    if (ota->record.rollbacks < UINT8_MAX) ++ota->record.rollbacks;
    ota->record.offset = 0;
    ota->written = ota->published = 0;
    if (ota->record.rollbacks >= BASE_OTA_MAX_ROLLBACKS) {
        finish(ota, BASE_OTA_FAILED, "rollback");
    } else {
        ota->record.phase = BASE_OTA_DOWNLOADING;
        set_error(&ota->record, "rollback");
    }
    return BASE_OTA_ACT_PERSIST;
}

unsigned base_ota_on_confirmed(base_ota_t *ota)
{
    ota->confirmed = true;
    if (ota->record.phase == BASE_OTA_REBOOTING && !strcmp(ota->record.sha256, ota->running.sha256)) {
        ota->record.offset = ota->written = ota->record.size;
        finish(ota, BASE_OTA_SUCCEEDED, NULL);
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH | reconcile(ota);
    }
    return BASE_OTA_ACT_PUBLISH | reconcile(ota);
}

unsigned base_ota_on_desired(base_ota_t *ota, const base_ota_desired_t *desired)
{
    ota->desired = *desired;
    ota->have_desired = true;
    if (ota->worker_running) {
        /* 换了任务或制品，且还没切换启动槽：中止当前下载，结束后再按新期望处理。 */
        const bool changed = strcmp(ota->record.job, desired->job) ||
                             strcmp(ota->record.sha256, desired->sha256);
        return changed ? BASE_OTA_ACT_CANCEL : 0;
    }
    return reconcile(ota);
}

unsigned base_ota_on_invalid_desired(base_ota_t *ota, const char *job, const char *error)
{
    ota->have_desired = false;
    if (!strcmp(ota->record.job, job) && terminal(ota->record.phase)) return BASE_OTA_ACT_PUBLISH;
    if (ota->record.phase == BASE_OTA_REBOOTING) return 0;
    unsigned actions = BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    if (ota->worker_running) actions |= BASE_OTA_ACT_CANCEL;
    if (ota->retry_pending) {
        ota->retry_pending = false;
        actions |= BASE_OTA_ACT_STOP_TIMER;
    }
    clear(ota);
    memcpy(ota->record.job, job, sizeof ota->record.job);
    finish(ota, BASE_OTA_REJECTED, error);
    return actions;
}

unsigned base_ota_on_cancel(base_ota_t *ota)
{
    ota->have_desired = false;
    /* 已切换启动槽或已有终态：不再改动。 */
    if (ota->record.phase != BASE_OTA_DOWNLOADING && ota->record.phase != BASE_OTA_VERIFYING) return 0;
    unsigned actions = 0;
    if (ota->retry_pending) {
        ota->retry_pending = false;
        actions |= BASE_OTA_ACT_STOP_TIMER;
    }
    /* 下载任务在跑：等它结束后再清记录。 */
    if (ota->worker_running) return actions | BASE_OTA_ACT_CANCEL;
    clear(ota);
    return actions | BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
}

unsigned base_ota_on_progress(base_ota_t *ota, uint32_t written)
{
    if (!ota->worker_running || written > ota->record.size || written < ota->written) return 0;
    unsigned actions = 0;
    ota->written = written;
    const uint32_t durable = written - written % BASE_OTA_PERSIST_STEP_BYTES;
    if (durable > ota->record.offset) {
        ota->record.offset = durable;
        actions |= BASE_OTA_ACT_PERSIST;
    }
    if (written - ota->published >= ota->record.size / 20u) {
        ota->published = written;
        actions |= BASE_OTA_ACT_PUBLISH;
    }
    return actions;
}

unsigned base_ota_on_verifying(base_ota_t *ota)
{
    if (!ota->worker_running || ota->record.phase != BASE_OTA_DOWNLOADING) return 0;
    ota->record.phase = BASE_OTA_VERIFYING;
    ota->written = ota->record.size;
    return BASE_OTA_ACT_PUBLISH;
}

unsigned base_ota_on_worker_done(base_ota_t *ota, base_ota_result_t result, const char *error)
{
    if (!ota->worker_running) return 0;
    ota->worker_running = false;
    /* record 仍描述这次下载；否则它已被新期望取代（例如新期望被拒绝）。 */
    const bool owns = ota->record.phase == BASE_OTA_DOWNLOADING || ota->record.phase == BASE_OTA_VERIFYING;
    if (result == BASE_OTA_RESULT_SWITCHED) {
        /* 启动槽已切换：即使期望已变也先重启，新固件上线后再收敛到新期望。 */
        if (!owns) return BASE_OTA_ACT_REBOOT;
        ota->record.offset = ota->written = ota->record.size;
        finish(ota, BASE_OTA_REBOOTING, NULL);
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH | BASE_OTA_ACT_REBOOT;
    }
    if (!owns) return reconcile(ota);
    if (!ota->have_desired) {
        /* 期望已取消。 */
        clear(ota);
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    }
    if (strcmp(ota->record.job, ota->desired.job) || strcmp(ota->record.sha256, ota->desired.sha256))
        return reconcile(ota); /* 期望换了任务或制品 */
    if (result == BASE_OTA_RESULT_REJECT) {
        finish(ota, BASE_OTA_REJECTED, error ? error : "rejected");
        return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH;
    }
    /* 网络类错误或被无故中止：退避后从已落盘偏移续传。 */
    ota->record.phase = BASE_OTA_DOWNLOADING;
    set_error(&ota->record, error ? error : "transport");
    if (ota->written > ota->attempt_start) ota->fail_streak = 1; /* 有进展：退避回到最短 */
    else if (ota->fail_streak < UINT8_MAX) ++ota->fail_streak;
    ota->written = ota->published = ota->record.offset;
    ota->retry_pending = true;
    return BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH | BASE_OTA_ACT_RETRY_TIMER;
}

unsigned base_ota_on_retry_timer(base_ota_t *ota)
{
    if (!ota->retry_pending) return 0;
    ota->retry_pending = false;
    if (ota->record.phase != BASE_OTA_DOWNLOADING) return 0;
    return reconcile(ota);
}

uint32_t base_ota_retry_delay_ms(const base_ota_t *ota, uint32_t random)
{
    uint32_t delay = BASE_OTA_RETRY_MIN_MS;
    for (uint8_t i = 1; i < ota->fail_streak && delay < BASE_OTA_RETRY_MAX_MS; ++i) delay *= 2u;
    if (delay > BASE_OTA_RETRY_MAX_MS) delay = BASE_OTA_RETRY_MAX_MS;
    return delay + random % (delay / 5u + 1u);
}

const char *base_ota_phase_name(base_ota_phase_t phase)
{
    static const char *const names[] = {"idle", "downloading", "verifying", "rebooting",
                                        "succeeded", "failed", "rejected"};
    return (unsigned)phase < sizeof names / sizeof *names ? names[phase] : "idle";
}

int base_ota_state_encode(const base_ota_t *ota, char *out, size_t size)
{
    char job[BASE_UUID_BYTES + 2] = "null", error[BASE_ERROR_BYTES + 2] = "null";
    if (ota->record.job[0]) snprintf(job, sizeof job, "\"%s\"", ota->record.job);
    if (ota->record.error[0]) snprintf(error, sizeof error, "\"%s\"", ota->record.error);
    const bool live = ota->record.phase == BASE_OTA_DOWNLOADING || ota->record.phase == BASE_OTA_VERIFYING;
    const int n = snprintf(out, size,
        "{\"job\":%s,\"state\":\"%s\",\"offset\":%lu,\"attempt\":%u,\"error\":%s,"
        "\"running\":{\"version\":\"%s\",\"sha256\":\"%s\",\"target\":\"%s\",\"slot\":\"%s\"}}",
        job, base_ota_phase_name((base_ota_phase_t)ota->record.phase),
        (unsigned long)(live ? ota->written : ota->record.offset), (unsigned)ota->record.attempt, error,
        ota->running.version, ota->running.sha256, ota->running.target, ota->running.slot);
    return n > 0 && (size_t)n < size ? n : -1;
}
