// SPDX-License-Identifier: Apache-2.0
// host 测试：应用部署的状态机。首次部署、替换、试运行失败回退、试运行期间设备重启、断电续做、
// 重试退避、取消、换任务、拒绝，以及 app/desired 的解码和 app/state 的编码。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wasm_deploy.h"

static int s_failures, s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { ++s_failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

#define JOB_A "6ba7b810-9dad-41d1-80b4-00c04fd430c8"
#define JOB_B "6ba7b810-9dad-41d1-80b4-00c04fd430c9"
#define JOB_C "6ba7b810-9dad-41d1-80b4-00c04fd430ca"
#define SHA_X "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
#define SHA_Y "2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae"
#define SHA_Z "fcde2b2edba56bf408601fb721fe9b5c338d10ee429ea04fae5511b68fbf8fb9"
#define SLOT_BYTES 32768u

static const wasm_slot_t EMPTY[WASM_SLOTS];
static const wasm_app_status_t APP = {.state = "running", .pool_used = 5832, .pool_peak = 5900, .linear_bytes = 4176};
#define APP_JSON ",\"app\":{\"state\":\"running\",\"restarts\":0,\"fault\":null,\"pool_used\":5832,\"pool_peak\":5900,\"linear\":4176},\"abi\":1}"

static wasm_desired_t desired(const char *job, const char *sha256)
{
    wasm_desired_t want = {.size = 8192};
    strcpy(want.job, job);
    strcpy(want.sha256, sha256);
    strcpy(want.url, "https://apps.example.test/app/x.ebapp?e=1&s=2");
    return want;
}

static wasm_slot_t package(const char *sha256, const char *version)
{
    wasm_slot_t slot = {.valid = true, .abi = 1};
    strcpy(slot.sha256, sha256);
    strcpy(slot.name, "hello");
    strcpy(slot.version, version);
    return slot;
}

/* 把一个任务从期望应用一路做到提交。 */
static void deploy_ok(wasm_deploy_t *deploy, const char *job, const char *sha256, const char *version)
{
    const wasm_desired_t want = desired(job, sha256);
    const wasm_slot_t installed = package(sha256, version);
    CHECK(wasm_deploy_on_desired(deploy, &want) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(wasm_deploy_on_verifying(deploy) == WASM_ACT_PUBLISH);
    CHECK(wasm_deploy_on_worker_done(deploy, WASM_WORKER_INSTALLED, NULL, &installed) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN));
    CHECK(wasm_deploy_on_started(deploy, true, NULL) == WASM_ACT_TRIAL_TIMER);
    CHECK(wasm_deploy_on_trial_timer(deploy) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
}

static void test_first_deployment(void)
{
    wasm_deploy_t deploy;
    char json[WASM_DEPLOY_STATE_BYTES];
    /* 没装过应用的设备：不运行、不写 NVS。 */
    CHECK(wasm_deploy_boot(&deploy, NULL, EMPTY, false) == 0);
    CHECK(deploy.running == WASM_SLOT_NONE && deploy.record.active == WASM_SLOT_NONE);
    CHECK(wasm_deploy_state_encode(&deploy, &APP, json, sizeof json) > 0);
    CHECK(!strcmp(json, "{\"job\":null,\"state\":\"idle\",\"attempt\":0,\"error\":null,\"running\":null" APP_JSON));

    const wasm_desired_t want = desired(JOB_A, SHA_X);
    CHECK(wasm_deploy_on_desired(&deploy, &want) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(deploy.record.phase == WASM_DEPLOY_DOWNLOADING && deploy.record.attempt == 1 && wasm_deploy_target(&deploy) == 0);
    /* 进行中的任务再次送达（重连后的 retained）：什么也不做。 */
    CHECK(wasm_deploy_on_desired(&deploy, &want) == 0);
    CHECK(wasm_deploy_on_verifying(&deploy) == WASM_ACT_PUBLISH && deploy.record.phase == WASM_DEPLOY_VERIFYING);

    const wasm_slot_t installed = package(SHA_X, "0.1.0+1");
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &installed) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN));
    CHECK(deploy.record.phase == WASM_DEPLOY_STARTING && deploy.record.trial == 0 && deploy.running == 0);
    CHECK(deploy.record.active == WASM_SLOT_NONE); /* 还没提交 */
    CHECK(wasm_deploy_on_started(&deploy, true, NULL) == WASM_ACT_TRIAL_TIMER);
    CHECK(wasm_deploy_on_trial_timer(&deploy) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
    CHECK(deploy.record.phase == WASM_DEPLOY_SUCCEEDED && deploy.record.active == 0 && deploy.record.trial == WASM_SLOT_NONE);
    CHECK(wasm_deploy_state_encode(&deploy, &APP, json, sizeof json) > 0);
    CHECK(!strcmp(json, "{\"job\":\"" JOB_A "\",\"state\":\"succeeded\",\"attempt\":1,\"error\":null,"
                        "\"running\":{\"name\":\"hello\",\"version\":\"0.1.0+1\",\"sha256\":\"" SHA_X "\",\"abi\":1}" APP_JSON));
    char small[64];
    CHECK(wasm_deploy_state_encode(&deploy, &APP, small, sizeof small) == -1);

    /* 同一个任务再送达：只重报结果。提交后的故障不回退。 */
    CHECK(wasm_deploy_on_desired(&deploy, &want) == WASM_ACT_PUBLISH);
    CHECK(wasm_deploy_on_fault(&deploy, "unreachable") == 0 && deploy.running == 0);
    CHECK(wasm_deploy_on_started(&deploy, false, "load") == 0);
    /* 新任务要的就是正在运行的包：不下载，直接成功。 */
    const wasm_desired_t again = desired(JOB_B, SHA_X);
    CHECK(wasm_deploy_on_desired(&deploy, &again) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
    CHECK(deploy.record.phase == WASM_DEPLOY_SUCCEEDED && !strcmp(deploy.record.job, JOB_B) && deploy.record.attempt == 0);
}

static void test_replace_and_rollback(void)
{
    wasm_deploy_t deploy;
    wasm_deploy_boot(&deploy, NULL, EMPTY, false);
    deploy_ok(&deploy, JOB_A, SHA_X, "0.1.0+1");

    /* 新版本下载到另一个槽；启动失败：回到原来的槽，任务失败。 */
    const wasm_desired_t next = desired(JOB_B, SHA_Y);
    const wasm_slot_t installed = package(SHA_Y, "0.1.0+2");
    CHECK(wasm_deploy_on_desired(&deploy, &next) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(wasm_deploy_target(&deploy) == 1);
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &installed) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN));
    CHECK(deploy.running == 1 && deploy.record.active == 0);
    CHECK(wasm_deploy_on_started(&deploy, false, "app_init") ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN | WASM_ACT_STOP_TRIAL));
    CHECK(deploy.record.phase == WASM_DEPLOY_FAILED && !strcmp(deploy.record.error, "app_init"));
    CHECK(deploy.running == 0 && deploy.record.active == 0 && deploy.record.trial == WASM_SLOT_NONE);
    /* 失败的任务再送达：只重报，不重试。 */
    CHECK(wasm_deploy_on_desired(&deploy, &next) == WASM_ACT_PUBLISH);

    /* 试运行期间出故障：同样回退。 */
    const wasm_desired_t third = desired(JOB_C, SHA_Z);
    const wasm_slot_t faulty = package(SHA_Z, "0.1.0+3");
    CHECK(wasm_deploy_on_desired(&deploy, &third) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &faulty);
    CHECK(wasm_deploy_on_started(&deploy, true, NULL) == WASM_ACT_TRIAL_TIMER);
    CHECK(wasm_deploy_on_fault(&deploy, "instruction limit") ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN | WASM_ACT_STOP_TRIAL));
    CHECK(deploy.record.phase == WASM_DEPLOY_FAILED && deploy.running == 0);
    /* 回退之后试运行定时器到期：不再有后果。 */
    CHECK(wasm_deploy_on_trial_timer(&deploy) == 0 && deploy.record.active == 0);
}

static void test_replace_commits(void)
{
    wasm_deploy_t deploy;
    wasm_deploy_boot(&deploy, NULL, EMPTY, false);
    deploy_ok(&deploy, JOB_A, SHA_X, "0.1.0+1");
    deploy_ok(&deploy, JOB_B, SHA_Y, "0.1.0+2");
    CHECK(deploy.record.active == 1 && deploy.running == 1 && wasm_deploy_target(&deploy) == 0);
    deploy_ok(&deploy, JOB_C, SHA_Z, "0.1.0+3");
    CHECK(deploy.record.active == 0 && deploy.running == 0 && wasm_deploy_target(&deploy) == 1);
    /* 回到上一版：就是再部署一次那个包。 */
    CHECK(deploy.slots[1].valid && !strcmp(deploy.slots[1].sha256, SHA_Y));
}

static void test_boot(void)
{
    wasm_deploy_t deploy;
    wasm_slot_t slots[WASM_SLOTS] = {package(SHA_X, "0.1.0+1"), package(SHA_Y, "0.1.0+2")};

    /* 已提交的应用：照常运行，记录没变就不写。 */
    wasm_deploy_record_t record = {.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                   .phase = WASM_DEPLOY_SUCCEEDED, .attempt = 1, .job = JOB_A, .sha256 = SHA_X};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == WASM_ACT_RUN && deploy.running == 0);

    /* 试运行期间设备重启：第一次再给一次机会。 */
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = 1,
                                    .phase = WASM_DEPLOY_STARTING, .attempt = 1, .job = JOB_B, .sha256 = SHA_Y};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == (WASM_ACT_RUN | WASM_ACT_PERSIST));
    CHECK(deploy.running == 1 && deploy.record.trial == 1 && deploy.record.trial_boots == 1);
    CHECK(wasm_deploy_on_started(&deploy, true, NULL) == WASM_ACT_TRIAL_TIMER);
    /* 第二次重启：认定是新应用拖垮了设备，回到原来的应用。 */
    record = deploy.record;
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == (WASM_ACT_RUN | WASM_ACT_PERSIST));
    CHECK(deploy.running == 0 && deploy.record.trial == WASM_SLOT_NONE && deploy.record.phase == WASM_DEPLOY_FAILED &&
          !strcmp(deploy.record.error, "rollback"));
    const wasm_desired_t again = desired(JOB_B, SHA_Y);
    CHECK(wasm_deploy_on_desired(&deploy, &again) == WASM_ACT_PUBLISH); /* 同一个任务不再重试 */

    /* 首次部署的试运行把设备拖垮：回退后没有应用可运行。 */
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = WASM_SLOT_NONE, .trial = 0,
                                    .trial_boots = 1, .phase = WASM_DEPLOY_STARTING, .job = JOB_A, .sha256 = SHA_X};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == WASM_ACT_PERSIST && deploy.running == WASM_SLOT_NONE);

    /* 下载途中断电：任务回到待做，同一个任务再送达时从头下载。 */
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                    .phase = WASM_DEPLOY_DOWNLOADING, .attempt = 3, .job = JOB_B, .sha256 = SHA_Y};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == (WASM_ACT_RUN | WASM_ACT_PERSIST));
    CHECK(deploy.record.phase == WASM_DEPLOY_IDLE && deploy.running == 0);
    CHECK(wasm_deploy_on_desired(&deploy, &again) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(deploy.record.attempt == 1);

    /* 已提交的槽读不出有效的包（被擦、被改坏、固件换了签名键）：没有应用可运行。 */
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                    .phase = WASM_DEPLOY_SUCCEEDED, .job = JOB_A, .sha256 = SHA_X};
    CHECK(wasm_deploy_boot(&deploy, &record, EMPTY, false) == WASM_ACT_PERSIST);
    CHECK(deploy.running == WASM_SLOT_NONE && deploy.record.active == WASM_SLOT_NONE);

    /* 记录本身不可用（版本不认识、槽号越界）：当作没有记录，并重写。 */
    record = (wasm_deploy_record_t){.version = 9, .active = 0, .trial = WASM_SLOT_NONE};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == WASM_ACT_PERSIST && deploy.running == WASM_SLOT_NONE);
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = 7, .trial = WASM_SLOT_NONE};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == WASM_ACT_PERSIST && deploy.running == WASM_SLOT_NONE);
}

static void test_retry_cancel_and_switch(void)
{
    wasm_deploy_t deploy;
    wasm_deploy_boot(&deploy, NULL, EMPTY, false);
    const wasm_desired_t want = desired(JOB_A, SHA_X);
    wasm_deploy_on_desired(&deploy, &want);

    /* 网络类错误：退避后重试，5 秒起每次翻倍，上限 5 分钟。 */
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_RETRY, "stalled", NULL) == (WASM_ACT_PUBLISH | WASM_ACT_RETRY_TIMER));
    CHECK(deploy.record.phase == WASM_DEPLOY_DOWNLOADING && !strcmp(deploy.record.error, "stalled"));
    CHECK(wasm_deploy_retry_delay_ms(&deploy, 0) == 5000);
    CHECK(wasm_deploy_on_retry_timer(&deploy) == (WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD) && deploy.record.attempt == 2);
    CHECK(wasm_deploy_on_retry_timer(&deploy) == 0); /* 定时器只算一次 */
    wasm_deploy_on_worker_done(&deploy, WASM_WORKER_RETRY, "network", NULL);
    CHECK(wasm_deploy_retry_delay_ms(&deploy, 0) == 10000 && wasm_deploy_retry_delay_ms(&deploy, 1999) == 11999);
    for (int round = 0; round < 12; ++round) {
        wasm_deploy_on_retry_timer(&deploy);
        wasm_deploy_on_worker_done(&deploy, WASM_WORKER_RETRY, "network", NULL);
    }
    CHECK(wasm_deploy_retry_delay_ms(&deploy, 0) == 300000);

    /* 等重试的时候取消：任务回到空闲。 */
    CHECK(wasm_deploy_on_cancel(&deploy) == (WASM_ACT_STOP_TIMER | WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
    CHECK(deploy.record.phase == WASM_DEPLOY_IDLE && wasm_deploy_on_retry_timer(&deploy) == 0);
    /* 取消之后同一个任务再发来：从头做。 */
    CHECK(wasm_deploy_on_desired(&deploy, &want) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));

    /* 下载途中取消：先让下载任务停下，它结束后回到空闲。 */
    CHECK(wasm_deploy_on_cancel(&deploy) == WASM_ACT_CANCEL && wasm_deploy_on_cancel(&deploy) == 0);
    CHECK(wasm_deploy_on_verifying(&deploy) == 0);
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_CANCELED, NULL, NULL) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
    CHECK(deploy.record.phase == WASM_DEPLOY_IDLE);

    /* 下载途中来了新任务：中止当前下载，结束后接着做新任务。 */
    wasm_deploy_on_desired(&deploy, &want);
    const wasm_desired_t next = desired(JOB_B, SHA_Y);
    CHECK(wasm_deploy_on_desired(&deploy, &next) == WASM_ACT_CANCEL);
    CHECK(wasm_deploy_on_desired(&deploy, &next) == 0);
    /* 下载任务恰好在收到中止之前装完了：仍按中止处理，装进去的包不启用。 */
    const wasm_slot_t late = package(SHA_X, "0.1.0+1");
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &late) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(!strcmp(deploy.record.job, JOB_B) && deploy.record.phase == WASM_DEPLOY_DOWNLOADING && deploy.running == WASM_SLOT_NONE);

    /* 等重试的时候来了新任务：直接换。 */
    wasm_deploy_on_worker_done(&deploy, WASM_WORKER_RETRY, "network", NULL);
    const wasm_desired_t third = desired(JOB_C, SHA_Z);
    CHECK(wasm_deploy_on_desired(&deploy, &third) == (WASM_ACT_STOP_TIMER | WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(!strcmp(deploy.record.job, JOB_C) && deploy.record.attempt == 1);

    /* 试运行期间来了新任务：等试运行出结果再做。 */
    const wasm_slot_t installed = package(SHA_Z, "0.1.0+3");
    wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &installed);
    wasm_deploy_on_started(&deploy, true, NULL);
    CHECK(wasm_deploy_on_desired(&deploy, &next) == 0 && deploy.record.phase == WASM_DEPLOY_STARTING);
    CHECK(wasm_deploy_on_cancel(&deploy) == 0); /* 已经装好的不取消；等着的新任务作废 */
    CHECK(wasm_deploy_on_desired(&deploy, &next) == 0);
    CHECK(wasm_deploy_on_trial_timer(&deploy) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(deploy.record.active == 0 && !strcmp(deploy.record.job, JOB_B) && wasm_deploy_target(&deploy) == 1);
}

static void test_reject(void)
{
    wasm_deploy_t deploy;
    wasm_deploy_boot(&deploy, NULL, EMPTY, false);
    const wasm_desired_t want = desired(JOB_A, SHA_X);
    wasm_deploy_on_desired(&deploy, &want);
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_REJECT, "abi_mismatch", NULL) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
    CHECK(deploy.record.phase == WASM_DEPLOY_REJECTED && !strcmp(deploy.record.error, "abi_mismatch"));
    CHECK(wasm_deploy_on_desired(&deploy, &want) == WASM_ACT_PUBLISH);
    char json[WASM_DEPLOY_STATE_BYTES];
    CHECK(wasm_deploy_state_encode(&deploy, &APP, json, sizeof json) > 0);
    CHECK(!strcmp(json, "{\"job\":\"" JOB_A "\",\"state\":\"rejected\",\"attempt\":1,\"error\":\"abi_mismatch\",\"running\":null" APP_JSON));
    /* 故障原因里的引号、反斜杠和控制字符不进 JSON。 */
    const wasm_app_status_t crashed = {.state = "crashed", .fault = "out of \"bounds\"\\\n access", .restarts = 3};
    CHECK(wasm_deploy_state_encode(&deploy, &crashed, json, sizeof json) > 0);
    CHECK(strstr(json, "\"app\":{\"state\":\"crashed\",\"restarts\":3,\"fault\":\"out of bounds access\",") != NULL);

    /* 字段无效的期望应用：记为拒绝；进行中的任务不被它打断。 */
    CHECK(wasm_deploy_on_invalid_desired(&deploy, JOB_B, "invalid_url") == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH));
    CHECK(deploy.record.phase == WASM_DEPLOY_REJECTED && !strcmp(deploy.record.job, JOB_B) && !strcmp(deploy.record.error, "invalid_url"));
    CHECK(wasm_deploy_on_invalid_desired(&deploy, JOB_B, "invalid_url") == WASM_ACT_PUBLISH);
    const wasm_desired_t next = desired(JOB_C, SHA_Z);
    wasm_deploy_on_desired(&deploy, &next);
    CHECK(wasm_deploy_on_invalid_desired(&deploy, JOB_A, "invalid_desired") == 0 && !strcmp(deploy.record.job, JOB_C));
    /* 装进去的包事后读不出来：按失败处理。 */
    const wasm_slot_t broken = {0};
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &broken) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN | WASM_ACT_STOP_TRIAL));
    CHECK(deploy.record.phase == WASM_DEPLOY_FAILED && deploy.running == WASM_SLOT_NONE);
}

#define DESIRED(job, size, sha, url) \
    "{\"job\":\"" job "\",\"name\":\"hello\",\"version\":\"0.1.0+1\",\"abi\":1,\"size\":" size ",\"sha256\":\"" sha "\",\"url\":\"" url "\"}"

static const char *decode(const char *json, wasm_desired_t *out)
{
    return wasm_desired_decode(json, strlen(json), SLOT_BYTES, out);
}

static void test_desired_decode(void)
{
    wasm_desired_t want;
    CHECK(decode(DESIRED(JOB_A, "8192", SHA_X, "https://apps.example.test/app/x.ebapp?e=1&s=2"), &want) == NULL);
    CHECK(!strcmp(want.job, JOB_A) && !strcmp(want.sha256, SHA_X) && want.size == 8192 &&
          !strcmp(want.url, "https://apps.example.test/app/x.ebapp?e=1&s=2"));
    CHECK(decode(DESIRED(JOB_A, "32768", SHA_X, "https://a.test/x"), &want) == NULL && want.size == 32768);

    /* 大小：整扇区，至少一个内容扇区加一个签名扇区，不超过槽。 */
    CHECK(!strcmp(decode(DESIRED(JOB_A, "36864", SHA_X, "https://a.test/x"), &want), "package_too_large") && !strcmp(want.job, JOB_A));
    CHECK(!strcmp(decode(DESIRED(JOB_A, "4096", SHA_X, "https://a.test/x"), &want), "invalid_desired"));
    CHECK(!strcmp(decode(DESIRED(JOB_A, "8193", SHA_X, "https://a.test/x"), &want), "invalid_desired"));
    CHECK(!strcmp(decode(DESIRED(JOB_A, "-8192", SHA_X, "https://a.test/x"), &want), "invalid_desired"));
    CHECK(!strcmp(decode(DESIRED(JOB_A, "\"8192\"", SHA_X, "https://a.test/x"), &want), "invalid_desired"));
    /* 地址：只接受 https。 */
    CHECK(!strcmp(decode(DESIRED(JOB_A, "8192", SHA_X, "http://a.test/x"), &want), "invalid_url") && !strcmp(want.job, JOB_A));
    /* 摘要、任务号。任务号不对时连上报的对象都没有。 */
    CHECK(!strcmp(decode(DESIRED(JOB_A, "8192", "abc", "https://a.test/x"), &want), "invalid_desired") && !strcmp(want.job, JOB_A));
    CHECK(!strcmp(decode(DESIRED("not-a-uuid", "8192", SHA_X, "https://a.test/x"), &want), "invalid_desired") && !want.job[0]);
    /* 缺键、多余的键、不是对象、不是 JSON。 */
    CHECK(!strcmp(decode("{\"job\":\"" JOB_A "\",\"size\":8192,\"sha256\":\"" SHA_X "\",\"url\":\"https://a.test/x\"}", &want),
                  "invalid_desired") && !strcmp(want.job, JOB_A));
    CHECK(!strcmp(decode("{\"job\":\"" JOB_A "\",\"name\":\"hello\",\"version\":\"0.1.0+1\",\"abi\":1,\"size\":8192,\"sha256\":\""
                         SHA_X "\",\"url\":\"https://a.test/x\",\"extra\":1}", &want), "invalid_desired"));
    CHECK(!strcmp(decode("[]", &want), "invalid_desired") && !want.job[0]);
    CHECK(!strcmp(decode("{", &want), "invalid_desired") && !want.job[0]);
}

static void test_crash_loop(void)
{
    wasm_deploy_t deploy;
    wasm_slot_t slots[WASM_SLOTS] = {package(SHA_X, "0.1.0+1"), package(SHA_Y, "0.1.0+2")};
    wasm_deploy_record_t record = {.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                   .phase = WASM_DEPLOY_SUCCEEDED, .attempt = 1, .job = JOB_A, .sha256 = SHA_X};
    /* 已提交的应用运行时设备异常重启：第一次照常运行并记一次。 */
    CHECK(wasm_deploy_boot(&deploy, &record, slots, true) == (WASM_ACT_RUN | WASM_ACT_PERSIST));
    CHECK(deploy.running == 0 && deploy.record.crashes == 1 && !deploy.disabled);
    /* 稳定运行之后清零；之后的一次异常重启重新从一数起。 */
    CHECK(wasm_deploy_on_stable(&deploy) == WASM_ACT_PERSIST && deploy.record.crashes == 0);
    CHECK(wasm_deploy_on_stable(&deploy) == 0);
    record = deploy.record;
    CHECK(wasm_deploy_boot(&deploy, &record, slots, true) == (WASM_ACT_RUN | WASM_ACT_PERSIST) && deploy.record.crashes == 1);
    /* 正常重启也清零。 */
    record = deploy.record;
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == (WASM_ACT_RUN | WASM_ACT_PERSIST) && deploy.record.crashes == 0);

    /* 连续两次：这次启动不运行应用，宿主在线等新的部署。 */
    record = deploy.record;
    wasm_deploy_boot(&deploy, &record, slots, true);
    record = deploy.record;
    CHECK(wasm_deploy_boot(&deploy, &record, slots, true) == WASM_ACT_PERSIST);
    CHECK(deploy.disabled && deploy.running == WASM_SLOT_NONE && deploy.record.active == 0 && deploy.record.crashes == 2);
    CHECK(wasm_deploy_on_stable(&deploy) == 0);
    /* 停用期间再异常重启：保持停用，不再写。 */
    record = deploy.record;
    CHECK(wasm_deploy_boot(&deploy, &record, slots, true) == 0 && deploy.disabled);
    /* 部署新应用：下载到另一个槽，装好后停用解除，照常试运行。 */
    const wasm_desired_t want = desired(JOB_B, SHA_Z);
    const wasm_slot_t installed = package(SHA_Z, "0.1.0+3");
    CHECK(wasm_deploy_on_desired(&deploy, &want) == (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_DOWNLOAD));
    CHECK(wasm_deploy_target(&deploy) == 1);
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &installed) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN));
    CHECK(!deploy.disabled && deploy.record.crashes == 0 && deploy.running == 1);
    /* 正常重启（上电、远程重启）也解除停用：再给已提交的应用一次机会。 */
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                    .phase = WASM_DEPLOY_SUCCEEDED, .crashes = 2, .job = JOB_A, .sha256 = SHA_X};
    CHECK(wasm_deploy_boot(&deploy, &record, slots, false) == (WASM_ACT_RUN | WASM_ACT_PERSIST) && !deploy.disabled);
}

/* 与 Server 共用的合同样例（docs/design/device-protocol-samples.json）里的应用部分。 */
/* 命令 app_restart。 */
static void test_restart(void)
{
    wasm_deploy_t deploy;
    wasm_slot_t slots[WASM_SLOTS] = {package(SHA_X, "0.1.0+1"), package(SHA_Y, "0.1.0+2")};
    wasm_slot_t empty[WASM_SLOTS] = {{0}, {0}};
    /* 没有应用：什么也不做。 */
    wasm_deploy_boot(&deploy, NULL, empty, false);
    CHECK(wasm_deploy_on_restart(&deploy) == 0);
    /* 已提交的应用在运行：停掉再启动，记录不变。 */
    wasm_deploy_record_t record = {.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                   .phase = WASM_DEPLOY_SUCCEEDED, .attempt = 1, .job = JOB_A, .sha256 = SHA_X};
    wasm_deploy_boot(&deploy, &record, slots, false);
    const wasm_deploy_record_t before = deploy.record;
    CHECK(wasm_deploy_on_restart(&deploy) == (WASM_ACT_RUN | WASM_ACT_PUBLISH) && deploy.running == 0);
    CHECK(!memcmp(&before, &deploy.record, sizeof before));
    CHECK(wasm_deploy_on_started(&deploy, true, NULL) == 0);
    /* 重启后启动失败：已提交的应用不回退，由宿主退避重试。 */
    CHECK(wasm_deploy_on_restart(&deploy) == (WASM_ACT_RUN | WASM_ACT_PUBLISH));
    CHECK(wasm_deploy_on_started(&deploy, false, "start_failed") == 0 && deploy.running == 0);

    /* 因为连续把设备拖到异常重启而被停用的应用：再给一次机会，计数从头算。 */
    record.crashes = 1;
    wasm_deploy_boot(&deploy, &record, slots, true);
    CHECK(deploy.disabled && deploy.running == WASM_SLOT_NONE && deploy.record.crashes == 2);
    CHECK(wasm_deploy_on_restart(&deploy) == (WASM_ACT_PERSIST | WASM_ACT_RUN | WASM_ACT_PUBLISH));
    CHECK(!deploy.disabled && deploy.running == 0 && deploy.record.crashes == 0 && deploy.record.active == 0);
    CHECK(wasm_deploy_on_restart(&deploy) == (WASM_ACT_RUN | WASM_ACT_PUBLISH)); /* 第二次只是普通的重启 */

    /* 试运行中的应用也可以重启：启动失败照常回到原来的应用。 */
    record = (wasm_deploy_record_t){.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE,
                                    .phase = WASM_DEPLOY_SUCCEEDED, .attempt = 1, .job = JOB_A, .sha256 = SHA_X};
    wasm_deploy_boot(&deploy, &record, slots, false);
    const wasm_desired_t want = desired(JOB_B, SHA_Y);
    wasm_deploy_on_desired(&deploy, &want);
    CHECK(wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &slots[1]) ==
          (WASM_ACT_PERSIST | WASM_ACT_PUBLISH | WASM_ACT_RUN));
    wasm_deploy_on_started(&deploy, true, NULL);
    CHECK(deploy.running == 1 && deploy.record.phase == WASM_DEPLOY_STARTING);
    CHECK(wasm_deploy_on_restart(&deploy) == (WASM_ACT_RUN | WASM_ACT_PUBLISH) && deploy.running == 1);
    const unsigned rolled_back = wasm_deploy_on_started(&deploy, false, "start_failed");
    CHECK((rolled_back & WASM_ACT_RUN) && deploy.running == 0 && deploy.record.phase == WASM_DEPLOY_FAILED);
}

static void test_contract_samples(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) exit(2);
    text[size] = '\0';
    fclose(file);
    cJSON *root = cJSON_Parse(text);
    free(text);
    CHECK(root != NULL);
    const cJSON *sample;
    int desired_count = 0, state_count = 0;

    /* 期望应用：每个样例标明设备应当接受、以什么原因拒绝，还是连任务号都读不出来而丢弃。 */
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "app_desired")) {
        const char *expect = cJSON_GetObjectItem(sample, "expect")->valuestring;
        char *payload = cJSON_PrintUnformatted(cJSON_GetObjectItem(sample, "payload"));
        wasm_desired_t want;
        const char *error = wasm_desired_decode(payload, strlen(payload), SLOT_BYTES, &want);
        const bool as_expected = !strcmp(expect, "ok")       ? !error && want.job[0] && want.size && want.url[0]
                                 : !strcmp(expect, "ignore") ? error && !want.job[0]
                                                             : error && !strcmp(error, expect) && want.job[0];
        if (!as_expected) fprintf(stderr, "app_desired sample %s: got %s\n", cJSON_GetObjectItem(sample, "name")->valuestring, error ? error : "ok");
        CHECK(as_expected);
        free(payload);
        ++desired_count;
    }
    CHECK(desired_count >= 8);

    /* 应用状态：样例里的每一份都必须是设备会发出的形状。 */
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "app_state")) {
        const cJSON *payload = cJSON_GetObjectItem(sample, "payload");
        const cJSON *job = cJSON_GetObjectItem(payload, "job"), *state = cJSON_GetObjectItem(payload, "state");
        const cJSON *error = cJSON_GetObjectItem(payload, "error"), *running = cJSON_GetObjectItem(payload, "running");
        const cJSON *app = cJSON_GetObjectItem(payload, "app");
        bool phase_known = false, app_known = false;
        for (int phase = WASM_DEPLOY_IDLE; phase <= WASM_DEPLOY_REJECTED; ++phase)
            phase_known = phase_known || (cJSON_IsString(state) && !strcmp(state->valuestring, wasm_deploy_phase_name(phase)));
        static const char *const app_states[] = {"running", "stopped", "crashed", "disabled"};
        const cJSON *app_state = cJSON_GetObjectItem(app, "state");
        for (int index = 0; index < 4; ++index)
            app_known = app_known || (cJSON_IsString(app_state) && !strcmp(app_state->valuestring, app_states[index]));
        CHECK(cJSON_GetArraySize(payload) == 7 && (cJSON_IsNull(job) || cJSON_IsString(job)) && phase_known &&
              cJSON_IsNumber(cJSON_GetObjectItem(payload, "attempt")) && (cJSON_IsNull(error) || cJSON_IsString(error)) &&
              cJSON_IsNumber(cJSON_GetObjectItem(payload, "abi")));
        CHECK(cJSON_IsNull(running) ||
              (cJSON_GetArraySize(running) == 4 && cJSON_IsString(cJSON_GetObjectItem(running, "name")) &&
               cJSON_IsString(cJSON_GetObjectItem(running, "version")) && cJSON_IsString(cJSON_GetObjectItem(running, "sha256")) &&
               cJSON_IsNumber(cJSON_GetObjectItem(running, "abi"))));
        CHECK(cJSON_GetArraySize(app) == 6 && app_known && cJSON_IsNumber(cJSON_GetObjectItem(app, "restarts")) &&
              (cJSON_IsNull(cJSON_GetObjectItem(app, "fault")) || cJSON_IsString(cJSON_GetObjectItem(app, "fault"))) &&
              cJSON_IsNumber(cJSON_GetObjectItem(app, "pool_used")) && cJSON_IsNumber(cJSON_GetObjectItem(app, "pool_peak")) &&
              cJSON_IsNumber(cJSON_GetObjectItem(app, "linear")));
        ++state_count;
    }
    CHECK(state_count >= 8);

    /* 其中两份由状态机逐字段重新生成：没装过应用的设备，和一次成功的替换。 */
    wasm_deploy_t deploy;
    char json[WASM_DEPLOY_STATE_BYTES];
    const wasm_app_status_t stopped = {.state = "stopped"};
    wasm_deploy_boot(&deploy, NULL, EMPTY, false);
    CHECK(wasm_deploy_state_encode(&deploy, &stopped, json, sizeof json) > 0);
    cJSON *encoded = cJSON_Parse(json);
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "app_state"))
        if (!strcmp(cJSON_GetObjectItem(sample, "name")->valuestring, "no_app"))
            CHECK(cJSON_Compare(encoded, cJSON_GetObjectItem(sample, "payload"), true));
    cJSON_Delete(encoded);

    wasm_slot_t slots[WASM_SLOTS] = {{.valid = true, .abi = 1, .name = "hxh_lift", .version = "1.0.0+11", .sha256 = SHA_X}};
    wasm_deploy_record_t record = {.version = WASM_DEPLOY_RECORD_VERSION, .active = 0, .trial = WASM_SLOT_NONE};
    wasm_deploy_boot(&deploy, &record, slots, false);
    wasm_desired_t want = desired(JOB_A, SHA_Y);
    wasm_slot_t installed = {.valid = true, .abi = 1, .name = "hxh_lift", .version = "1.0.0+12", .sha256 = SHA_Y};
    wasm_deploy_on_desired(&deploy, &want);
    wasm_deploy_on_worker_done(&deploy, WASM_WORKER_INSTALLED, NULL, &installed);
    wasm_deploy_on_started(&deploy, true, NULL);
    wasm_deploy_on_trial_timer(&deploy);
    const wasm_app_status_t running = {.state = "running", .pool_used = 9216, .pool_peak = 9480, .linear_bytes = 4296};
    CHECK(wasm_deploy_state_encode(&deploy, &running, json, sizeof json) > 0);
    encoded = cJSON_Parse(json);
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "app_state"))
        if (!strcmp(cJSON_GetObjectItem(sample, "name")->valuestring, "succeeded"))
            CHECK(cJSON_Compare(encoded, cJSON_GetObjectItem(sample, "payload"), true));
    cJSON_Delete(encoded);
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    test_first_deployment();
    test_crash_loop();
    test_restart();
    test_replace_and_rollback();
    test_replace_commits();
    test_boot();
    test_retry_cancel_and_switch();
    test_reject();
    test_desired_decode();
    if (argc > 1) test_contract_samples(argv[1]);
    printf("wasm deploy tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
