// SPDX-License-Identifier: Apache-2.0
#include "wasm_deploy.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static bool slot_ok(uint8_t slot)
{
    return slot < WASM_SLOTS;
}

static bool in_flight(const wasm_deploy_t *deploy)
{
    const uint8_t phase = deploy->record.phase;
    return phase == WASM_DEPLOY_DOWNLOADING || phase == WASM_DEPLOY_VERIFYING || phase == WASM_DEPLOY_STARTING;
}

static void set_error(wasm_deploy_t *deploy, const char *error)
{
    snprintf(deploy->record.error, sizeof deploy->record.error, "%s", error ? error : "");
}

uint8_t wasm_deploy_target(const wasm_deploy_t *deploy)
{
    return deploy->record.active == 0 ? 1 : 0;
}

/* 开始处理 deploy->desired 这个任务。 */
static unsigned begin(wasm_deploy_t *deploy)
{
    wasm_deploy_record_t *record = &deploy->record;
    deploy->deferred = false;
    deploy->fail_streak = 0;
    memset(record->job, 0, sizeof record->job);
    memset(record->sha256, 0, sizeof record->sha256);
    snprintf(record->job, sizeof record->job, "%s", deploy->desired.job);
    snprintf(record->sha256, sizeof record->sha256, "%s", deploy->desired.sha256);
    set_error(deploy, NULL);
    record->attempt = 0;
    /* 期望的就是已经提交的那个包：不用下载。 */
    if (slot_ok(record->active) && deploy->slots[record->active].valid &&
        !strcmp(deploy->slots[record->active].sha256, record->sha256)) {
        record->phase = WASM_DEPLOY_SUCCEEDED;
        return WASM_ACT_PERSIST | WASM_ACT_PUBLISH;
    }
    record->phase = WASM_DEPLOY_DOWNLOADING;
    record->attempt = 1;
    deploy->worker_running = true;
    return WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD;
}

/* 一个任务了结之后：有等着的新任务就接着做。 */
static unsigned settle(wasm_deploy_t *deploy, unsigned actions)
{
    return deploy->deferred && deploy->have_desired ? actions | begin(deploy) : actions;
}

static unsigned rollback(wasm_deploy_t *deploy, const char *error)
{
    wasm_deploy_record_t *record = &deploy->record;
    record->trial = WASM_SLOT_NONE;
    record->trial_boots = 0;
    record->phase = WASM_DEPLOY_FAILED;
    set_error(deploy, error);
    deploy->running = record->active;
    return settle(deploy, WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN | WASM_ACT_STOP_TRIAL);
}

unsigned wasm_deploy_boot(wasm_deploy_t *deploy, const wasm_deploy_record_t *stored, const wasm_slot_t slots[WASM_SLOTS],
                          bool abnormal)
{
    memset(deploy, 0, sizeof *deploy);
    memcpy(deploy->slots, slots, sizeof deploy->slots);
    wasm_deploy_record_t *record = &deploy->record;
    const bool usable = stored && stored->version == WASM_DEPLOY_RECORD_VERSION &&
                        (stored->active == WASM_SLOT_NONE || slot_ok(stored->active)) &&
                        (stored->trial == WASM_SLOT_NONE || slot_ok(stored->trial)) &&
                        stored->phase <= WASM_DEPLOY_REJECTED;
    if (usable) {
        *record = *stored;
        record->job[sizeof record->job - 1] = record->sha256[sizeof record->sha256 - 1] = '\0';
        record->error[sizeof record->error - 1] = '\0';
    } else {
        *record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = WASM_SLOT_NONE,
                                         .trial = WASM_SLOT_NONE};
    }
    const wasm_deploy_record_t before = *record;

    /* 槽里的包不在了（没写完、被改坏、固件换了签名键）：记录跟着作废。 */
    if (slot_ok(record->active) && !deploy->slots[record->active].valid) record->active = WASM_SLOT_NONE;
    if (slot_ok(record->trial) && (!deploy->slots[record->trial].valid || record->trial == record->active)) {
        record->trial = WASM_SLOT_NONE;
        record->trial_boots = 0;
        if (record->phase == WASM_DEPLOY_STARTING) {
            record->phase = WASM_DEPLOY_FAILED;
            set_error(deploy, "package_invalid");
        }
    }
    if (slot_ok(record->trial)) {
        /* 试运行期间设备重启了。第一次可能与应用无关，再给一次机会；第二次就回到原来的应用。 */
        if (++record->trial_boots >= WASM_TRIAL_BOOTS) {
            record->trial = WASM_SLOT_NONE;
            record->trial_boots = 0;
            record->phase = WASM_DEPLOY_FAILED;
            set_error(deploy, "rollback");
        }
    } else if (record->phase == WASM_DEPLOY_STARTING) {
        record->phase = WASM_DEPLOY_FAILED;
        set_error(deploy, "rollback");
    }
    /* 下载或校验途中断电：任务还在（retained 的期望应用会再送来），到时从头下载。 */
    if (record->phase == WASM_DEPLOY_DOWNLOADING || record->phase == WASM_DEPLOY_VERIFYING)
        record->phase = WASM_DEPLOY_IDLE;

    if (slot_ok(record->trial)) {
        deploy->running = record->trial;
    } else {
        /* 已提交的应用：上次是异常复位就记一次，连续到上限这次就不运行它，宿主照常在线等新的部署。
         * 正常重启（上电、远程重启、刷写）重新计数。 */
        if (!slot_ok(record->active) || !abnormal) record->crashes = 0;
        else if (record->crashes < WASM_CRASH_LIMIT) ++record->crashes;
        deploy->disabled = record->crashes >= WASM_CRASH_LIMIT;
        deploy->running = deploy->disabled ? WASM_SLOT_NONE : record->active;
    }
    unsigned actions = slot_ok(deploy->running) ? WASM_ACT_RUN : 0;
    /* 没有记录、也没有东西要记时不写 NVS：没装过应用的设备不产生写入。 */
    if (memcmp(&before, record, sizeof before) != 0 || (stored && !usable)) actions |= WASM_ACT_PERSIST;
    return actions;
}

unsigned wasm_deploy_on_restart(wasm_deploy_t *deploy)
{
    wasm_deploy_record_t *record = &deploy->record;
    unsigned actions = 0;
    if (deploy->disabled && slot_ok(record->active) && deploy->slots[record->active].valid) {
        deploy->disabled = false;
        deploy->running = record->active;
        record->crashes = 0;
        actions = WASM_ACT_PERSIST;
    }
    /* 试运行中的应用也可以重启：启动失败照常回退，试运行的计时不重新开始。 */
    return slot_ok(deploy->running) ? actions | WASM_ACT_RUN | WASM_ACT_PUBLISH : 0;
}

unsigned wasm_deploy_on_desired(wasm_deploy_t *deploy, const wasm_desired_t *desired)
{
    wasm_deploy_record_t *record = &deploy->record;
    const bool same_job = !strcmp(desired->job, record->job);
    deploy->desired = *desired;
    deploy->have_desired = true;
    if (same_job) {
        deploy->deferred = false;
        switch ((wasm_deploy_phase_t)record->phase) {
        case WASM_DEPLOY_SUCCEEDED:
        case WASM_DEPLOY_FAILED:
        case WASM_DEPLOY_REJECTED: return WASM_ACT_PUBLISH; /* 只重报原结果；重试由 Server 建新任务 */
        case WASM_DEPLOY_IDLE: return begin(deploy);        /* 上次没做完：断电，或取消之后又发来 */
        default:
            /* 正在进行。下载任务刚被要求中止时，等它结束后再从头来。 */
            deploy->deferred = deploy->canceling;
            return 0;
        }
    }
    if (!in_flight(deploy)) return begin(deploy);
    /* 另一个任务还没了结：还在下载就中止它，已经在试运行就等它出结果。 */
    deploy->deferred = true;
    if (record->phase == WASM_DEPLOY_STARTING) return 0;
    if (deploy->worker_running) {
        if (deploy->canceling) return 0;
        deploy->canceling = true;
        return WASM_ACT_CANCEL;
    }
    deploy->retry_pending = false; /* 正在等重试：直接换成新任务 */
    return WASM_ACT_STOP_TIMER | begin(deploy);
}

unsigned wasm_deploy_on_invalid_desired(wasm_deploy_t *deploy, const char *job, const char *error)
{
    wasm_deploy_record_t *record = &deploy->record;
    if (!strcmp(job, record->job)) return in_flight(deploy) ? 0 : WASM_ACT_PUBLISH;
    /* 别的任务还没了结时不打断它：retained 的期望应用下次上线还会送来。 */
    if (in_flight(deploy)) return 0;
    deploy->have_desired = deploy->deferred = false;
    memset(record->job, 0, sizeof record->job);
    memset(record->sha256, 0, sizeof record->sha256);
    snprintf(record->job, sizeof record->job, "%s", job);
    record->phase = WASM_DEPLOY_REJECTED;
    record->attempt = 0;
    set_error(deploy, error);
    return WASM_ACT_PERSIST | WASM_ACT_PUBLISH;
}

unsigned wasm_deploy_on_cancel(wasm_deploy_t *deploy)
{
    wasm_deploy_record_t *record = &deploy->record;
    deploy->have_desired = deploy->deferred = false;
    if (record->phase != WASM_DEPLOY_DOWNLOADING && record->phase != WASM_DEPLOY_VERIFYING) return 0;
    if (deploy->worker_running) {
        if (deploy->canceling) return 0;
        deploy->canceling = true;
        return WASM_ACT_CANCEL;
    }
    deploy->retry_pending = false;
    record->phase = WASM_DEPLOY_IDLE;
    set_error(deploy, NULL);
    return WASM_ACT_STOP_TIMER | WASM_ACT_PERSIST | WASM_ACT_PUBLISH;
}

unsigned wasm_deploy_on_verifying(wasm_deploy_t *deploy)
{
    if (deploy->record.phase != WASM_DEPLOY_DOWNLOADING || deploy->canceling) return 0;
    deploy->record.phase = WASM_DEPLOY_VERIFYING;
    return WASM_ACT_PUBLISH;
}

unsigned wasm_deploy_on_worker_done(wasm_deploy_t *deploy, wasm_worker_result_t result, const char *error,
                                    const wasm_slot_t *installed)
{
    wasm_deploy_record_t *record = &deploy->record;
    const bool canceling = deploy->canceling;
    deploy->worker_running = deploy->canceling = false;
    if (record->phase != WASM_DEPLOY_DOWNLOADING && record->phase != WASM_DEPLOY_VERIFYING) return 0;
    const uint8_t target = wasm_deploy_target(deploy);
    /* 目标槽被擦过、写过：不管结果如何，它原来的内容都不算数了。 */
    deploy->slots[target] = result == WASM_WORKER_INSTALLED && installed ? *installed : (wasm_slot_t){0};
    if (canceling || result == WASM_WORKER_CANCELED) {
        record->phase = WASM_DEPLOY_IDLE;
        set_error(deploy, NULL);
        return settle(deploy, WASM_ACT_PERSIST | WASM_ACT_PUBLISH);
    }
    switch (result) {
    case WASM_WORKER_INSTALLED:
        if (!deploy->slots[target].valid) return rollback(deploy, "package_invalid");
        record->trial = target;
        record->trial_boots = 0;
        record->crashes = 0;
        deploy->disabled = false;
        record->phase = WASM_DEPLOY_STARTING;
        set_error(deploy, NULL);
        deploy->fail_streak = 0;
        deploy->running = target;
        return WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN;
    case WASM_WORKER_RETRY:
        if (deploy->fail_streak < 255) ++deploy->fail_streak;
        record->phase = WASM_DEPLOY_DOWNLOADING;
        set_error(deploy, error);
        deploy->retry_pending = true;
        return WASM_ACT_PUBLISH | WASM_ACT_RETRY_TIMER;
    default:
        record->phase = WASM_DEPLOY_REJECTED;
        set_error(deploy, error ? error : "package_invalid");
        return settle(deploy, WASM_ACT_PERSIST | WASM_ACT_PUBLISH);
    }
}

unsigned wasm_deploy_on_retry_timer(wasm_deploy_t *deploy)
{
    wasm_deploy_record_t *record = &deploy->record;
    if (!deploy->retry_pending) return 0;
    deploy->retry_pending = false;
    if (record->phase != WASM_DEPLOY_DOWNLOADING || deploy->worker_running) return 0;
    if (record->attempt < 255) ++record->attempt;
    set_error(deploy, NULL);
    deploy->worker_running = true;
    return WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD;
}

static bool on_trial(const wasm_deploy_t *deploy)
{
    return deploy->record.phase == WASM_DEPLOY_STARTING && slot_ok(deploy->record.trial) &&
           deploy->running == deploy->record.trial;
}

unsigned wasm_deploy_on_started(wasm_deploy_t *deploy, bool ok, const char *error)
{
    if (!on_trial(deploy)) return 0; /* 已提交的应用启动失败由宿主退避重试，不在这里处理 */
    return ok ? WASM_ACT_TRIAL_TIMER : rollback(deploy, error ? error : "start_failed");
}

unsigned wasm_deploy_on_fault(wasm_deploy_t *deploy, const char *error)
{
    return on_trial(deploy) ? rollback(deploy, error ? error : "fault") : 0;
}

unsigned wasm_deploy_on_trial_timer(wasm_deploy_t *deploy)
{
    wasm_deploy_record_t *record = &deploy->record;
    if (!on_trial(deploy)) return 0;
    record->active = record->trial;
    record->trial = WASM_SLOT_NONE;
    record->trial_boots = 0;
    record->phase = WASM_DEPLOY_SUCCEEDED;
    set_error(deploy, NULL);
    return settle(deploy, WASM_ACT_PERSIST | WASM_ACT_PUBLISH);
}

unsigned wasm_deploy_on_stable(wasm_deploy_t *deploy)
{
    if (deploy->record.crashes == 0 || deploy->disabled) return 0;
    deploy->record.crashes = 0;
    return WASM_ACT_PERSIST;
}

uint32_t wasm_deploy_retry_delay_ms(const wasm_deploy_t *deploy, uint32_t random)
{
    uint32_t delay = WASM_DEPLOY_RETRY_MIN_MS;
    for (uint8_t step = 1; step < deploy->fail_streak && delay < WASM_DEPLOY_RETRY_MAX_MS; ++step) delay *= 2;
    if (delay > WASM_DEPLOY_RETRY_MAX_MS) delay = WASM_DEPLOY_RETRY_MAX_MS;
    return delay + random % (delay / 5 + 1);
}

const char *wasm_deploy_phase_name(wasm_deploy_phase_t phase)
{
    static const char *const names[] = {"idle", "downloading", "verifying", "starting", "succeeded", "failed", "rejected"};
    return phase <= WASM_DEPLOY_REJECTED ? names[phase] : "idle";
}

/* ---------- app/desired 与 app/state ---------- */

static bool is_hex(const char *text, size_t length)
{
    if (strlen(text) != length) return false;
    for (size_t index = 0; index < length; ++index)
        if (!((text[index] >= '0' && text[index] <= '9') || (text[index] >= 'a' && text[index] <= 'f'))) return false;
    return true;
}

static bool is_uuid(const char *text)
{
    if (strlen(text) != 36) return false;
    for (int index = 0; index < 36; ++index) {
        const char c = text[index];
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

const char *wasm_desired_decode(const char *payload, size_t length, uint32_t slot_bytes, wasm_desired_t *out)
{
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_ParseWithLength(payload, length);
    const char *error = "invalid_desired";
    if (cJSON_IsObject(root)) {
        const cJSON *job = cJSON_GetObjectItemCaseSensitive(root, "job");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON *abi = cJSON_GetObjectItemCaseSensitive(root, "abi");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
        const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        /* 任务号先取出来：其余字段不对时也能把结果报回给这个任务。 */
        if (cJSON_IsString(job) && is_uuid(job->valuestring)) memcpy(out->job, job->valuestring, 37);
        const double bytes = cJSON_IsNumber(size) ? size->valuedouble : -1;
        /* name、version、abi 只供人看：设备以包里的清单为准。 */
        if (!out->job[0] || cJSON_GetArraySize(root) != 7 || !cJSON_IsString(name) || !cJSON_IsString(version) ||
            !cJSON_IsNumber(abi) || !cJSON_IsString(sha256) || !is_hex(sha256->valuestring, 64) || !cJSON_IsString(url) ||
            bytes < 2 * WASM_PACKAGE_SECTOR_BYTES || bytes != (double)(uint32_t)bytes ||
            (uint32_t)bytes % WASM_PACKAGE_SECTOR_BYTES != 0) {
            error = "invalid_desired";
        } else if ((uint32_t)bytes > slot_bytes) {
            error = "package_too_large";
        } else if (strncmp(url->valuestring, "https://", 8) != 0 || strlen(url->valuestring) >= sizeof out->url) {
            error = "invalid_url";
        } else {
            memcpy(out->sha256, sha256->valuestring, 65);
            out->size = (uint32_t)bytes;
            snprintf(out->url, sizeof out->url, "%s", url->valuestring);
            error = NULL;
        }
    }
    cJSON_Delete(root);
    return error;
}

/* 故障原因来自运行时的异常文本：只留可打印的 ASCII，去掉引号和反斜杠，免得破坏 JSON。 */
static void quote_fault(char *out, size_t size, const char *fault)
{
    size_t at = 0;
    if (!fault || !fault[0] || size < 8) {
        snprintf(out, size, "null");
        return;
    }
    out[at++] = '"';
    for (; *fault && at + 2 < size; ++fault)
        if (*fault >= 0x20 && *fault < 0x7f && *fault != '"' && *fault != '\\') out[at++] = *fault;
    out[at++] = '"';
    out[at] = '\0';
}

int wasm_deploy_state_encode(const wasm_deploy_t *deploy, const wasm_app_status_t *app, char *out, size_t size)
{
    const wasm_deploy_record_t *record = &deploy->record;
    char job[WASM_UUID_BYTES + 2] = "null", error[WASM_ERROR_BYTES + 2] = "null", running[224] = "null", fault[72];
    quote_fault(fault, sizeof fault, app->fault);
    if (record->job[0]) snprintf(job, sizeof job, "\"%s\"", record->job);
    if (record->error[0]) snprintf(error, sizeof error, "\"%s\"", record->error);
    if (slot_ok(deploy->running) && deploy->slots[deploy->running].valid) {
        const wasm_slot_t *slot = &deploy->slots[deploy->running];
        snprintf(running, sizeof running, "{\"name\":\"%s\",\"version\":\"%s\",\"sha256\":\"%s\",\"abi\":%u}", slot->name,
                 slot->version, slot->sha256, (unsigned)slot->abi);
    }
    const int length = snprintf(out, size,
                                "{\"job\":%s,\"state\":\"%s\",\"attempt\":%u,\"error\":%s,\"running\":%s,"
                                "\"app\":{\"state\":\"%s\",\"restarts\":%u,\"fault\":%s,\"pool_used\":%u,\"pool_peak\":%u,"
                                "\"linear\":%u},\"abi\":%u}",
                                job, wasm_deploy_phase_name((wasm_deploy_phase_t)record->phase), (unsigned)record->attempt,
                                error, running, app->state, (unsigned)app->restarts, fault, (unsigned)app->pool_used,
                                (unsigned)app->pool_peak, (unsigned)app->linear_bytes, (unsigned)WASM_ABI);
    return length > 0 && (size_t)length < size ? length : -1;
}
