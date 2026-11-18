// SPDX-License-Identifier: Apache-2.0
// host 测试：协议编解码、去重、OTA 状态机，以及与 Server 共用的合同样例。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_ota.h"
#include "base_protocol.h"
#include "cJSON.h"

static int s_failures, s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { ++s_failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

#define DEVICE "0f8fad5b-d9cb-469f-a165-70867728950e"
#define JOB_A "6ba7b810-9dad-41d1-80b4-00c04fd430c8"
#define JOB_B "6ba7b810-9dad-41d1-80b4-00c04fd430c9"
#define SHA_RUNNING "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
#define SHA_NEW "2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae"
#define SHA_OTHER "fcde2b2edba56bf408601fb721fe9b5c338d10ee429ea04fae5511b68fbf8fb9"
#define SIZE 1052672u
#define SLOT 0x1e0000u

static const base_fw_t RUNNING = {.version = "1.0.0+68", .sha256 = SHA_RUNNING,
                                  .target = "esp32c3/esp_base", .slot = "ota_0"};

static base_ota_desired_t desired(const char *job, const char *sha256)
{
    base_ota_desired_t d = {.size = SIZE};
    strcpy(d.job, job);
    strcpy(d.sha256, sha256);
    strcpy(d.version, "1.0.0+69");
    strcpy(d.target, "esp32c3/esp_base");
    strcpy(d.url, "https://firmware.example.test/fw/esp32c3/x.bin?e=1&s=2");
    return d;
}

static void test_topics(void)
{
    char topic[BASE_TOPIC_BYTES], name[BASE_CMD_NAME_BYTES];
    CHECK(base_topic(topic, DEVICE, BASE_TOPIC_STATUS) && !strcmp(topic, "esp-base/" DEVICE "/status"));
    CHECK(base_topic(topic, DEVICE, BASE_TOPIC_CMD_FILTER) && !strcmp(topic, "esp-base/" DEVICE "/cmd/+"));
    CHECK(base_topic(topic, DEVICE, BASE_TOPIC_OTA_DESIRED) && !strcmp(topic, "esp-base/" DEVICE "/ota/desired"));
    CHECK(!base_topic(topic, "not-a-uuid", BASE_TOPIC_STATUS));
    const char *restart = "esp-base/" DEVICE "/cmd/restart";
    CHECK(base_topic_command(restart, strlen(restart), DEVICE, name) && !strcmp(name, "restart"));
    const char *other = "esp-base/7c9e6679-7425-40de-944b-e07fc1f90ae7/cmd/restart";
    CHECK(!base_topic_command(other, strlen(other), DEVICE, name)); /* 别的设备 */
    const char *nested = "esp-base/" DEVICE "/cmd/a/b";
    CHECK(!base_topic_command(nested, strlen(nested), DEVICE, name));
    const char *empty = "esp-base/" DEVICE "/cmd/";
    CHECK(!base_topic_command(empty, strlen(empty), DEVICE, name));
}

static void test_dedupe(void)
{
    base_dedupe_t dedupe = {0};
    char response[BASE_RSP_BYTES], id[BASE_UUID_BYTES];
    for (int i = 0; i < BASE_DEDUPE_SLOTS + 1; ++i) {
        snprintf(id, sizeof id, "3f2504e0-4f89-41d3-9a0c-0305e82c33%02x", i);
        CHECK(base_rsp_encode(response, id, "status", NULL) > 0);
        base_dedupe_store(&dedupe, id, response);
    }
    CHECK(base_dedupe_find(&dedupe, "3f2504e0-4f89-41d3-9a0c-0305e82c3300") == NULL); /* 最旧的被挤出 */
    const char *kept = base_dedupe_find(&dedupe, "3f2504e0-4f89-41d3-9a0c-0305e82c3308");
    CHECK(kept && strstr(kept, "\"ok\":true"));
    CHECK(base_dedupe_find(&dedupe, "") == NULL);
}

/* config 只改 Wi-Fi 和业务 MQTT；业务 MQTT 解码到独立字段，系统 MQTT 的 mqtt 字段整条拒绝。 */
static void test_config_decode(void)
{
    base_cmd_t command;
    const char *error = NULL;
    const char *business = "{\"request_id\":\"3f2504e0-4f89-41d3-9a0c-0305e82c3304\","
        "\"business_mqtt\":{\"uri\":\"mqtts://business.example.test:8883\",\"username\":\"u\",\"password\":\"p\"}}";
    CHECK(base_cmd_decode("config", business, strlen(business), &command, &error) && !error);
    CHECK(command.kind == BASE_CMD_CONFIG && !command.config.has_wifi && command.config.has_business_mqtt);
    CHECK(!strcmp(command.config.business_mqtt.uri, "mqtts://business.example.test:8883") &&
          !strcmp(command.config.business_mqtt.username, "u") && !strcmp(command.config.business_mqtt.password, "p"));
    const char *system = "{\"request_id\":\"3f2504e0-4f89-41d3-9a0c-0305e82c3308\","
        "\"wifi\":{\"ssid\":\"lab\",\"password\":\"password-1\"},"
        "\"mqtt\":{\"uri\":\"mqtts://broker.example.test:8883\",\"username\":\"u\",\"password\":\"p\"}}";
    CHECK(!base_cmd_decode("config", system, strlen(system), &command, &error) &&
          command.request_id[0] && error && !strcmp(error, "invalid_args"));
    /* 名称和 Group：group 为 null 表示移出。 */
    const char *profile = "{\"request_id\":\"3f2504e0-4f89-41d3-9a0c-0305e82c3311\",\"name\":\"Lab C3\","
        "\"group\":\"1b4e28ba-2fa1-41d2-883f-0016d3cca427\"}";
    CHECK(base_cmd_decode("config", profile, strlen(profile), &command, &error) && !error);
    CHECK(command.config.has_name && !strcmp(command.config.name, "Lab C3") && command.config.has_group &&
          !strcmp(command.config.group, "1b4e28ba-2fa1-41d2-883f-0016d3cca427") && !command.config.has_wifi);
    const char *leave = "{\"request_id\":\"3f2504e0-4f89-41d3-9a0c-0305e82c3312\",\"group\":null}";
    CHECK(base_cmd_decode("config", leave, strlen(leave), &command, &error) && !error);
    CHECK(command.config.has_group && !command.config.group[0] && !command.config.has_name);
    const char *number = "{\"request_id\":\"3f2504e0-4f89-41d3-9a0c-0305e82c3313\",\"name\":7}";
    CHECK(!base_cmd_decode("config", number, strlen(number), &command, &error) && error && !strcmp(error, "invalid_args"));
}

/* 新任务：开始下载；下载途中偏移每 64 KiB 落盘；每 5% 发布；完成后记录 rebooting 并重启。 */
static void test_ota_happy_path(void)
{
    base_ota_t ota;
    CHECK(base_ota_boot(&ota, NULL, &RUNNING, SLOT, true) == 0);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    unsigned actions = base_ota_on_desired(&ota, &d);
    CHECK(actions == (BASE_OTA_ACT_START | BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH));
    CHECK(ota.record.phase == BASE_OTA_DOWNLOADING && ota.record.attempt == 1 && ota.record.offset == 0);
    CHECK(base_ota_on_desired(&ota, &d) == 0); /* 同一期望重复到达：不重复启动 */
    CHECK(base_ota_on_progress(&ota, 4096) == 0);
    actions = base_ota_on_progress(&ota, 65536 + 100);
    CHECK((actions & BASE_OTA_ACT_PERSIST) && ota.record.offset == 65536);
    CHECK(actions & BASE_OTA_ACT_PUBLISH); /* 超过 5% */
    CHECK(base_ota_on_progress(&ota, 65536 + 200) == 0);
    CHECK(base_ota_on_progress(&ota, SIZE + 1) == 0); /* 越界的进度被忽略 */
    CHECK(base_ota_on_verifying(&ota) == BASE_OTA_ACT_PUBLISH && ota.record.phase == BASE_OTA_VERIFYING);
    actions = base_ota_on_worker_done(&ota, BASE_OTA_RESULT_SWITCHED, NULL);
    CHECK(actions == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH | BASE_OTA_ACT_REBOOT));
    CHECK(ota.record.phase == BASE_OTA_REBOOTING && ota.record.offset == SIZE);

    /* 新固件启动：连上 Broker 之前不记成功。 */
    base_ota_record_t stored = ota.record;
    base_fw_t updated = RUNNING;
    strcpy(updated.sha256, SHA_NEW);
    CHECK(base_ota_boot(&ota, &stored, &updated, SLOT, false) == 0);
    CHECK(ota.record.phase == BASE_OTA_REBOOTING);
    actions = base_ota_on_confirmed(&ota);
    CHECK(actions == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH) && ota.record.phase == BASE_OTA_SUCCEEDED);
    CHECK(base_ota_on_desired(&ota, &d) == BASE_OTA_ACT_PUBLISH); /* 期望重投：只回报 */
    char state[448];
    CHECK(base_ota_state_encode(&ota, state, sizeof state) > 0 && strstr(state, "\"state\":\"succeeded\""));
}

/* 运行中的固件就是期望固件：直接成功，不下载。 */
static void test_ota_already_running(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_RUNNING);
    CHECK(base_ota_on_desired(&ota, &d) == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH));
    CHECK(ota.record.phase == BASE_OTA_SUCCEEDED && !ota.worker_running && !strcmp(ota.record.job, JOB_A));
}

/* 断电续传：记录里是 downloading，上电后同一任务从已存偏移继续。 */
static void test_ota_resume_after_power_loss(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    base_ota_on_desired(&ota, &d);
    base_ota_on_progress(&ota, 5 * 65536 + 1234);
    base_ota_record_t stored = ota.record; /* 此刻断电 */
    CHECK(stored.offset == 5 * 65536);

    CHECK(base_ota_boot(&ota, &stored, &RUNNING, SLOT, true) == 0);
    CHECK(base_ota_on_confirmed(&ota) == BASE_OTA_ACT_PUBLISH); /* 还没收到期望：只上报 */
    unsigned actions = base_ota_on_desired(&ota, &d);
    CHECK(actions & BASE_OTA_ACT_START);
    CHECK(ota.record.offset == 5 * 65536 && ota.record.attempt == 2 && ota.written == 5 * 65536);

    /* 校验中断电：回到下载。 */
    stored.phase = BASE_OTA_VERIFYING;
    CHECK(base_ota_boot(&ota, &stored, &RUNNING, SLOT, true) == BASE_OTA_ACT_PERSIST);
    CHECK(ota.record.phase == BASE_OTA_DOWNLOADING);
}

/* 网络错误：退避后续传；有进展则退避回到最短；没有总期限。 */
static void test_ota_retry_backoff(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    base_ota_on_desired(&ota, &d);
    uint32_t previous = 0;
    for (int i = 1; i <= 12; ++i) {
        unsigned actions = base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "transport");
        CHECK(actions == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH | BASE_OTA_ACT_RETRY_TIMER));
        CHECK(ota.record.phase == BASE_OTA_DOWNLOADING && !strcmp(ota.record.error, "transport"));
        const uint32_t delay = base_ota_retry_delay_ms(&ota, 0);
        CHECK(delay >= BASE_OTA_RETRY_MIN_MS && delay <= BASE_OTA_RETRY_MAX_MS && delay >= previous);
        if (i == 1) CHECK(delay == 5000);
        if (i == 3) CHECK(delay == 20000);
        if (i >= 8) CHECK(delay == BASE_OTA_RETRY_MAX_MS);
        CHECK(base_ota_retry_delay_ms(&ota, 0xffffffffu) <= delay + delay / 5u);
        previous = delay;
        CHECK(base_ota_on_retry_timer(&ota) & BASE_OTA_ACT_START);
    }
    CHECK(ota.record.attempt == 13); /* 一直重试，不因次数失败 */
    base_ota_on_progress(&ota, 2 * 65536);
    base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "transport");
    CHECK(base_ota_retry_delay_ms(&ota, 0) == BASE_OTA_RETRY_MIN_MS); /* 有进展 */
    CHECK(ota.record.offset == 2 * 65536);
    /* Broker 重连后期望重投：立即续传并停掉定时器。 */
    unsigned actions = base_ota_on_desired(&ota, &d);
    CHECK((actions & BASE_OTA_ACT_START) && (actions & BASE_OTA_ACT_STOP_TIMER));
    CHECK(base_ota_on_retry_timer(&ota) == 0);
}

/* 断线或停滞：续传点精确到扇区；只有续传点前进才算有进展；整个镜像写完后的失败不把续传点推到末尾。 */
static void test_ota_resume_after_stall(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    base_ota_on_desired(&ota, &d);
    base_ota_on_progress(&ota, 65536 + 100);
    CHECK(ota.record.offset == 65536);
    CHECK(base_ota_on_progress(&ota, 65536 + 3 * 4096 + 123) == 0); /* 途中不为扇区落盘 */
    unsigned actions = base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "stalled");
    CHECK(actions == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH | BASE_OTA_ACT_RETRY_TIMER));
    CHECK(ota.record.offset == 65536 + 3 * 4096 && !strcmp(ota.record.error, "stalled"));
    CHECK(ota.written == ota.record.offset && base_ota_retry_delay_ms(&ota, 0) == BASE_OTA_RETRY_MIN_MS);
    CHECK(base_ota_on_retry_timer(&ota) & BASE_OTA_ACT_START);
    CHECK(ota.record.attempt == 2 && ota.attempt_start == 65536 + 3 * 4096);

    /* 这一次没写满一个扇区就停了：续传点不动，记为无进展，退避开始翻倍。 */
    base_ota_on_progress(&ota, 65536 + 3 * 4096 + 4095);
    base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "stalled");
    CHECK(ota.record.offset == 65536 + 3 * 4096 && base_ota_retry_delay_ms(&ota, 0) == 2 * BASE_OTA_RETRY_MIN_MS);
    CHECK(base_ota_on_retry_timer(&ota) & BASE_OTA_ACT_START);
    base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "connect");
    CHECK(base_ota_retry_delay_ms(&ota, 0) == 4 * BASE_OTA_RETRY_MIN_MS);
    CHECK(base_ota_on_retry_timer(&ota) & BASE_OTA_ACT_START);

    /* 断电后从落盘的续传点继续，它可以不在 64 KiB 边界上。 */
    base_ota_record_t stored = ota.record;
    CHECK(base_ota_boot(&ota, &stored, &RUNNING, SLOT, true) == 0);
    base_ota_on_confirmed(&ota);
    CHECK((base_ota_on_desired(&ota, &d) & BASE_OTA_ACT_START) && ota.written == 65536 + 3 * 4096);

    /* 整个镜像已写入但读回失败：保持原续传点，下次重新取回最后一段。 */
    base_ota_on_progress(&ota, SIZE);
    const uint32_t durable = ota.record.offset;
    CHECK(durable < SIZE && durable % 65536 == 0);
    base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "flash_read");
    CHECK(ota.record.offset == durable && ota.written == durable);
}

/* 拒绝：target 不符、超出槽容量、字段无效、下载任务判定不可恢复。设备不重启。 */
static void test_ota_reject(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    strcpy(d.target, "esp32/esp_base");
    CHECK(base_ota_on_desired(&ota, &d) == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH));
    CHECK(ota.record.phase == BASE_OTA_REJECTED && !strcmp(ota.record.error, "target_mismatch"));
    CHECK(base_ota_on_desired(&ota, &d) == BASE_OTA_ACT_PUBLISH); /* 同一任务：只回报原结果 */

    d = desired(JOB_B, SHA_NEW);
    d.size = SLOT + 1;
    base_ota_on_desired(&ota, &d);
    CHECK(ota.record.phase == BASE_OTA_REJECTED && !strcmp(ota.record.error, "image_too_large"));

    CHECK(base_ota_on_invalid_desired(&ota, JOB_A, "invalid_url") == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH));
    CHECK(ota.record.phase == BASE_OTA_REJECTED && !strcmp(ota.record.job, JOB_A) &&
          !strcmp(ota.record.error, "invalid_url"));

    const char *const reasons[] = {"sha256_mismatch", "image_invalid", "size_mismatch", "http_403", "http_404"};
    for (size_t i = 0; i < sizeof reasons / sizeof *reasons; ++i) {
        base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
        d = desired(JOB_A, SHA_NEW);
        CHECK(base_ota_on_desired(&ota, &d) & BASE_OTA_ACT_START);
        unsigned actions = base_ota_on_worker_done(&ota, BASE_OTA_RESULT_REJECT, reasons[i]);
        CHECK(actions == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH)); /* 没有 REBOOT */
        CHECK(ota.record.phase == BASE_OTA_REJECTED && !strcmp(ota.record.error, reasons[i]));
        CHECK(base_ota_on_desired(&ota, &d) == BASE_OTA_ACT_PUBLISH); /* 不重试 */
        base_ota_desired_t retry = desired(JOB_B, SHA_NEW);          /* Server 建新任务才重试 */
        CHECK(base_ota_on_desired(&ota, &retry) & BASE_OTA_ACT_START);
    }
}

/* 回滚计数：切换后运行的不是新固件，记一次回滚并重试；累计 3 次记为 failed。 */
static void test_ota_rollback_count(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    base_ota_record_t stored;
    for (unsigned round = 1; round <= BASE_OTA_MAX_ROLLBACKS; ++round) {
        CHECK(base_ota_on_desired(&ota, &d) & BASE_OTA_ACT_START);
        base_ota_on_worker_done(&ota, BASE_OTA_RESULT_SWITCHED, NULL);
        stored = ota.record;
        /* bootloader 回滚：旧固件再次启动。 */
        CHECK(base_ota_boot(&ota, &stored, &RUNNING, SLOT, true) == BASE_OTA_ACT_PERSIST);
        CHECK(ota.record.rollbacks == round && ota.record.offset == 0);
        if (round < BASE_OTA_MAX_ROLLBACKS) {
            CHECK(ota.record.phase == BASE_OTA_DOWNLOADING && !strcmp(ota.record.error, "rollback"));
            CHECK(base_ota_on_confirmed(&ota) == BASE_OTA_ACT_PUBLISH);
        }
    }
    CHECK(ota.record.phase == BASE_OTA_FAILED && !strcmp(ota.record.error, "rollback"));
    CHECK(base_ota_on_confirmed(&ota) == BASE_OTA_ACT_PUBLISH);
    CHECK(base_ota_on_desired(&ota, &d) == BASE_OTA_ACT_PUBLISH); /* 同一任务不再重试 */
    base_ota_desired_t retry = desired(JOB_B, SHA_NEW);
    CHECK(base_ota_on_desired(&ota, &retry) & BASE_OTA_ACT_START);
    CHECK(ota.record.rollbacks == 0);

    /* 新固件自检期间（未确认）不开始新的下载。 */
    base_fw_t updated = RUNNING;
    strcpy(updated.sha256, SHA_NEW);
    stored = ota.record;
    stored.phase = BASE_OTA_REBOOTING;
    base_ota_boot(&ota, &stored, &updated, SLOT, false);
    base_ota_desired_t next = desired(JOB_A, SHA_OTHER);
    CHECK(base_ota_on_desired(&ota, &next) == 0 && !ota.worker_running);
    CHECK(base_ota_on_confirmed(&ota) & BASE_OTA_ACT_START); /* 确认后收敛到最新期望 */
    CHECK(!strcmp(ota.record.job, JOB_A) && !strcmp(ota.record.sha256, SHA_OTHER));
}

/* 取消与换任务：未切换启动槽时中止；已切换则先重启。 */
static void test_ota_cancel_and_supersede(void)
{
    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_desired_t d = desired(JOB_A, SHA_NEW);
    base_ota_on_desired(&ota, &d);
    CHECK(base_ota_on_cancel(&ota) == BASE_OTA_ACT_CANCEL);
    CHECK(base_ota_on_worker_done(&ota, BASE_OTA_RESULT_CANCELED, NULL) == (BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH));
    CHECK(ota.record.phase == BASE_OTA_IDLE && ota.record.job[0] == '\0');
    CHECK(base_ota_on_cancel(&ota) == 0);

    /* 退避等待中取消。 */
    base_ota_on_desired(&ota, &d);
    base_ota_on_worker_done(&ota, BASE_OTA_RESULT_RETRY, "transport");
    unsigned actions = base_ota_on_cancel(&ota);
    CHECK(actions == (BASE_OTA_ACT_STOP_TIMER | BASE_OTA_ACT_PERSIST | BASE_OTA_ACT_PUBLISH));
    CHECK(ota.record.phase == BASE_OTA_IDLE && base_ota_on_retry_timer(&ota) == 0);

    /* 下载中换任务：中止后按新任务开始。 */
    base_ota_on_desired(&ota, &d);
    base_ota_desired_t next = desired(JOB_B, SHA_OTHER);
    CHECK(base_ota_on_desired(&ota, &next) == BASE_OTA_ACT_CANCEL);
    actions = base_ota_on_worker_done(&ota, BASE_OTA_RESULT_CANCELED, NULL);
    CHECK((actions & BASE_OTA_ACT_START) && !strcmp(ota.record.job, JOB_B) && ota.record.offset == 0);

    /* 取消到达前已切换启动槽：照常重启。 */
    CHECK(base_ota_on_cancel(&ota) == BASE_OTA_ACT_CANCEL);
    actions = base_ota_on_worker_done(&ota, BASE_OTA_RESULT_SWITCHED, NULL);
    CHECK((actions & BASE_OTA_ACT_REBOOT) && ota.record.phase == BASE_OTA_REBOOTING);
    CHECK(base_ota_on_cancel(&ota) == 0);

    /* 下载中新期望字段无效：记录新任务被拒绝，旧下载中止后不覆盖这条记录。 */
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    base_ota_on_desired(&ota, &d);
    actions = base_ota_on_invalid_desired(&ota, JOB_B, "invalid_url");
    CHECK((actions & BASE_OTA_ACT_CANCEL) && ota.record.phase == BASE_OTA_REJECTED);
    CHECK(base_ota_on_worker_done(&ota, BASE_OTA_RESULT_CANCELED, NULL) == 0);
    CHECK(ota.record.phase == BASE_OTA_REJECTED && !strcmp(ota.record.job, JOB_B));

    /* 损坏的持久记录不被采用。 */
    base_ota_record_t broken = {.version = 9, .phase = BASE_OTA_REBOOTING};
    CHECK(base_ota_boot(&ota, &broken, &RUNNING, SLOT, true) == 0 && ota.record.phase == BASE_OTA_IDLE);
}

/* ---- 合同样例：与 Server 共用同一份 JSON ---- */
static bool same_shape(const cJSON *expected, const cJSON *actual)
{
    if (cJSON_IsNull(expected) || cJSON_IsNull(actual)) return true; /* 可空字段 */
    if ((expected->type & 0xff) != (actual->type & 0xff) &&
        !(cJSON_IsBool(expected) && cJSON_IsBool(actual))) return false;
    if (!cJSON_IsObject(expected)) return true;
    if (cJSON_GetArraySize(expected) != cJSON_GetArraySize(actual)) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item, expected) {
        const cJSON *other = cJSON_GetObjectItemCaseSensitive(actual, item->string);
        if (!other || !same_shape(item, other)) return false;
    }
    return true;
}

/* 与样例 online 对应的 Profile；unnamed 为真时对应 online_unnamed。 */
static void profile_fixture(base_profile_t *p, bool unnamed)
{
    memset(p, 0, sizeof *p);
    strcpy(p->device_id, DEVICE);
    strcpy(p->boot_id, "7c9e6679-7425-40de-944b-e07fc1f90ae7");
    strcpy(p->reset, unnamed ? "software" : "power_on");
    p->fw = RUNNING;
    strcpy(p->chip_model, "esp32c3");
    strcpy(p->chip_revision, "v0.4");
    p->cores = 1;
    p->cpu_mhz = 160;
    p->chip_features = BASE_CHIP_WIFI | BASE_CHIP_BLE;
    p->flash_bytes = 4194304;
    strcpy(p->idf, "v6.1");
    strcpy(p->mac, "02:00:00:4a:1b:2c");
    strcpy(p->mqtt_uri, "mqtts://broker.example.test:8883");
    p->heap_free = 178148;
    p->heap_min = 160696;
    p->heap_largest = 110592;
    strcpy(p->ssid, "example-ssid");
    strcpy(p->bssid, "02:00:00:00:00:01");
    strcpy(p->hostname, "espressif");
    if (unnamed) {
        p->uptime_s = 42;
        p->rssi = -70;
        p->channel = 11;
        p->config_revision = 4;
        strcpy(p->config_error, "trial_failed");
        p->wakeups = 9;
        return;
    }
    p->uptime_s = 3600;
    strcpy(p->name, "\xe5\xae\x9e\xe9\xaa\x8c\xe6\x9d\xbf \"C3\""); /* 实验板 "C3"：UTF-8 原样，引号转义 */
    strcpy(p->group, "1b4e28ba-2fa1-41d2-883f-0016d3cca427");
    strcpy(p->business_mqtt_uri, "mqtts://business.example.test:8883");
    static const struct { const char *name; uint32_t bytes; } stack[] = {
        {"main", 4200}, {"mqtt_task", 2900}, {"mqtt_task_2", 3100}, {"IDLE", 800}};
    for (size_t i = 0; i < sizeof stack / sizeof *stack; ++i) {
        strcpy(p->stack[i].name, stack[i].name);
        p->stack[i].bytes = stack[i].bytes;
    }
    p->stack_count = 4;
    p->rssi = -52;
    p->channel = 6;
    p->has_ipv4 = true;
    strcpy(p->ipv4, "192.0.2.20");
    strcpy(p->netmask, "255.255.255.0");
    strcpy(p->gateway, "192.0.2.1");
    strcpy(p->ipv6[0], "fe80::ff:fe4a:1b2c");
    strcpy(p->ipv6[1], "2001:db8::ff:fe4a:1b2c");
    p->ipv6_count = 2;
    p->config_revision = 3;
    p->wakeups = 57;
}

/* 名称规则、默认名称和编码缓冲不足。 */
static void test_profile(void)
{
    char name[BASE_NAME_BYTES];
    base_default_name(name, "esp32c3", "02:00:00:4a:1b:2c");
    CHECK(!strcmp(name, "ESP32-C3-4A1B2C"));
    base_default_name(name, "esp32", "24:0a:c4:00:ab:cd");
    CHECK(!strcmp(name, "ESP32-00ABCD"));
    base_default_name(name, "esp32s3", "");
    CHECK(!strcmp(name, "ESP32-S3"));
    CHECK(base_is_device_name("12345678901234567890123456789012"));
    CHECK(!base_is_device_name("123456789012345678901234567890123"));
    CHECK(!base_is_device_name("") && !base_is_device_name("lab ") && !base_is_device_name("a\tb"));
    CHECK(base_is_device_name("\xe5\xae\x9e\xe9\xaa\x8c\xe6\x9d\xbf C3"));

    static base_profile_t profile;
    profile_fixture(&profile, false);
    char small[256], status[2560];
    CHECK(base_status_encode(&profile, small, sizeof small) == -1);
    const int n = base_status_encode(&profile, status, sizeof status);
    CHECK(n > 0 && (size_t)n == strlen(status));
    cJSON *parsed = cJSON_Parse(status);
    CHECK(parsed && !strcmp(cJSON_GetObjectItem(parsed, "name")->valuestring, "\xe5\xae\x9e\xe9\xaa\x8c\xe6\x9d\xbf \"C3\""));
    cJSON_Delete(parsed);
    /* 控制字符在 JSON 里转义，不会截断文档。 */
    strcpy(profile.ssid, "a\x01" "b"); /* \x01b 会被当成一个十六进制转义 */
    CHECK(base_status_encode(&profile, status, sizeof status) > 0 && strstr(status, "\"a\\u0001b\""));
    /* 栈槽位写满、IPv6 写满时仍放得下。 */
    for (int i = 0; i < BASE_STACK_SLOTS; ++i) {
        snprintf(profile.stack[i].name, BASE_TASK_NAME_BYTES, "task_name_%02d_x", i);
        profile.stack[i].bytes = 99999;
    }
    profile.stack_count = BASE_STACK_SLOTS;
    for (int i = 0; i < BASE_IPV6_SLOTS; ++i) strcpy(profile.ipv6[i], "2001:db8:1234:5678:9abc:def0:1234:5678");
    profile.ipv6_count = BASE_IPV6_SLOTS;
    memset(profile.business_mqtt_uri, 'b', sizeof profile.business_mqtt_uri - 1);
    memset(profile.mqtt_uri, 'm', sizeof profile.mqtt_uri - 1);
    CHECK(base_status_encode(&profile, status, sizeof status) > 0);
    char will[640];
    memset(profile.name, '"', BASE_NAME_BYTES - 1);
    CHECK(base_will_encode(&profile, will, sizeof will) > 0);
}

static void test_contract_samples(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file) return;
    static char text[32768];
    const size_t length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    cJSON *root = cJSON_ParseWithLength(text, length);
    CHECK(cJSON_IsObject(root));
    const char *device = cJSON_GetObjectItem(root, "device_id")->valuestring;
    const cJSON *topics = cJSON_GetObjectItem(root, "topics"), *sample;
    static const char *const names[] = {"status", "rsp", "cmd_filter", "ota_desired", "ota_state"};
    for (int i = 0; i < 5; ++i) {
        char topic[BASE_TOPIC_BYTES];
        CHECK(base_topic(topic, device, (base_topic_t)i) &&
              !strcmp(topic, cJSON_GetObjectItem(topics, names[i])->valuestring));
    }

    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "commands")) {
        const char *expect = cJSON_GetObjectItem(sample, "expect")->valuestring;
        char *payload = cJSON_PrintUnformatted(cJSON_GetObjectItem(sample, "payload"));
        base_cmd_t command;
        const char *error = NULL;
        const bool ok = base_cmd_decode(cJSON_GetObjectItem(sample, "cmd")->valuestring, payload,
                                        strlen(payload), &command, &error);
        if (!strcmp(expect, "ok")) CHECK(ok && !error);
        else if (!strcmp(expect, "drop")) CHECK(!ok && !command.request_id[0]);
        else CHECK(!ok && command.request_id[0] && error && !strcmp(expect + strlen("error:"), error));
        free(payload);
    }

    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "ota_desired")) {
        const char *expect = cJSON_GetObjectItem(sample, "expect")->valuestring;
        char *payload = cJSON_PrintUnformatted(cJSON_GetObjectItem(sample, "payload"));
        base_ota_desired_t d;
        const char *error = NULL;
        const base_desired_result_t result = base_ota_desired_decode(payload, strlen(payload), &d, &error);
        if (!strcmp(expect, "ok")) CHECK(result == BASE_DESIRED_OK && d.size == 1052672 && d.url[0]);
        else if (!strcmp(expect, "invalid")) CHECK(result == BASE_DESIRED_INVALID);
        else CHECK(result == BASE_DESIRED_REJECT && d.job[0] && !strcmp(expect + strlen("reject:"), error));
        free(payload);
    }
    base_ota_desired_t none;
    const char *error = NULL;
    CHECK(base_ota_desired_decode("", 0, &none, &error) == BASE_DESIRED_EMPTY); /* 空载荷 = 取消 */

    /* 设备编码的应答和 OTA 状态，字段与样例一致。 */
    char response[BASE_RSP_BYTES];
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "responses")) {
        const cJSON *payload = cJSON_GetObjectItem(sample, "payload");
        const cJSON *sample_error = cJSON_GetObjectItem(payload, "error");
        CHECK(base_rsp_encode(response, cJSON_GetObjectItem(payload, "request_id")->valuestring,
                              cJSON_GetObjectItem(payload, "cmd")->valuestring,
                              cJSON_IsNull(sample_error) ? NULL : sample_error->valuestring) > 0);
        cJSON *encoded = cJSON_Parse(response);
        CHECK(encoded && cJSON_Compare(encoded, payload, true));
        cJSON_Delete(encoded);
    }
    /* 设备按 Profile 编码的状态文档与遗嘱，与样例逐字段相同。 */
    static base_profile_t profile;
    char document[2560];
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "status")) {
        const char *sample_name = cJSON_GetObjectItem(sample, "name")->valuestring;
        const bool online = !strcmp(sample_name, "online"), unnamed = !strcmp(sample_name, "online_unnamed");
        const bool will = !strcmp(sample_name, "offline_will");
        if (!online && !unnamed && !will) continue; /* *_legacy 是旧固件的文档，只由 Server 解码 */
        profile_fixture(&profile, unnamed);
        const int n = will ? base_will_encode(&profile, document, sizeof document)
                           : base_status_encode(&profile, document, sizeof document);
        cJSON *encoded = n > 0 ? cJSON_Parse(document) : NULL;
        CHECK(encoded && cJSON_Compare(encoded, cJSON_GetObjectItem(sample, "payload"), true));
        if (!(encoded && cJSON_Compare(encoded, cJSON_GetObjectItem(sample, "payload"), true)))
            fprintf(stderr, "  %s: %s\n", sample_name, document);
        cJSON_Delete(encoded);
    }

    base_ota_t ota;
    base_ota_boot(&ota, NULL, &RUNNING, SLOT, true);
    char state[448];
    cJSON_ArrayForEach(sample, cJSON_GetObjectItem(root, "ota_state")) {
        const cJSON *payload = cJSON_GetObjectItem(sample, "payload");
        CHECK(base_ota_state_encode(&ota, state, sizeof state) > 0);
        cJSON *encoded = cJSON_Parse(state);
        CHECK(encoded && same_shape(payload, encoded));
        cJSON_Delete(encoded);
        bool known = false;
        for (int phase = BASE_OTA_IDLE; phase <= BASE_OTA_REJECTED; ++phase)
            known = known || !strcmp(base_ota_phase_name((base_ota_phase_t)phase),
                                     cJSON_GetObjectItem(payload, "state")->valuestring);
        CHECK(known);
    }
    /* idle 样例应与设备对空记录的编码逐字段相同。 */
    cJSON *encoded = cJSON_Parse(state);
    CHECK(encoded && cJSON_Compare(encoded, cJSON_GetObjectItem(cJSON_GetArrayItem(
        cJSON_GetObjectItem(root, "ota_state"), 0), "payload"), true));
    cJSON_Delete(encoded);
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    test_topics();
    test_dedupe();
    test_config_decode();
    test_profile();
    test_ota_happy_path();
    test_ota_already_running();
    test_ota_resume_after_power_loss();
    test_ota_retry_backoff();
    test_ota_resume_after_stall();
    test_ota_reject();
    test_ota_rollback_count();
    test_ota_cancel_and_supersede();
    if (argc > 1) test_contract_samples(argv[1]);
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
