// SPDX-License-Identifier: Apache-2.0
// host 测试：应用日志的编码（app/log），并用与 Server 共用的合同样例核对形状。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wasm_log.h"

static int s_failures, s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { ++s_failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

static char s_lines[WASM_LOG_LINES_BYTES];
static size_t s_length;

static bool append(uint64_t uptime_ms, int level, const char *text)
{
    return wasm_log_append(s_lines, sizeof s_lines, &s_length, uptime_ms, level, text, strlen(text));
}

/* 编码成一条消息并解析回来；调用方负责 cJSON_Delete。 */
static cJSON *message(uint32_t seq, uint32_t dropped)
{
    static char out[WASM_LOG_BYTES];
    const int length = wasm_log_encode(out, sizeof out, seq, dropped, s_lines, s_length);
    CHECK(length > 0 && (size_t)length == strlen(out));
    return cJSON_Parse(out);
}

static const char *text_of(const cJSON *root, int index)
{
    return cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "lines"), index), "text")->valuestring;
}

static void test_lines(void)
{
    s_length = 0;
    CHECK(append(1500, 3, "HXH_BOOT role=lift"));
    CHECK(append(2000, 2, "HXH_LEASE_EXPIRED stopping"));
    CHECK(append(4294967296000ull, 1, "boom"));
    CHECK(append(7, 0, "level 0 is an error too") && append(8, 9, "unknown levels are info"));
    cJSON *root = message(17, 3);
    CHECK(cJSON_IsObject(root) && cJSON_GetArraySize(root) == 3);
    CHECK(cJSON_GetObjectItem(root, "seq")->valueint == 17 && cJSON_GetObjectItem(root, "dropped")->valueint == 3);
    const cJSON *lines = cJSON_GetObjectItem(root, "lines");
    CHECK(cJSON_GetArraySize(lines) == 5);
    static const char *const levels[] = {"info", "warn", "error", "error", "info"};
    for (int index = 0; index < 5; ++index) {
        const cJSON *line = cJSON_GetArrayItem(lines, index);
        CHECK(cJSON_GetArraySize(line) == 3 && !strcmp(cJSON_GetObjectItem(line, "level")->valuestring, levels[index]));
    }
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(lines, 0), "t")->valuedouble == 1500);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(lines, 2), "t")->valuedouble == 4294967296000.0);
    CHECK(!strcmp(text_of(root, 0), "HXH_BOOT role=lift") && !strcmp(text_of(root, 2), "boom"));
    cJSON_Delete(root);

    /* 没有日志也能编码：一个空数组。 */
    s_length = 0;
    root = message(1, 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "lines")) == 0);
    cJSON_Delete(root);
}

static void test_escaping(void)
{
    s_length = 0;
    /* 引号、反斜杠、控制字符都转义；合法的多字节字符原样保留。 */
    CHECK(append(1, 3, "HXH_COMMAND move {\"direction\":\"up\"} \\ \t\n\x01\x7f 推杆 \xf0\x9f\x98\x80"));
    /* 不是合法 UTF-8 的字节换成 ?：孤立的续字节、过长编码的首字节、截断在半个字符上。 */
    CHECK(append(2, 3, "a\x80" "b\xc0\xaf" "c\xff" "d\xe6\x8e"));
    cJSON *root = message(1, 0);
    CHECK(cJSON_IsObject(root));
    CHECK(!strcmp(text_of(root, 0), "HXH_COMMAND move {\"direction\":\"up\"} \\ \t\n\x01\x7f 推杆 \xf0\x9f\x98\x80"));
    CHECK(!strcmp(text_of(root, 1), "a?b??c?d??"));
    cJSON_Delete(root);

    /* 正文最长 200 字节，超出的截掉；截在半个字符上时那半个换成 ?。 */
    static char text[400];
    memset(text, 'x', sizeof text - 1);
    s_length = 0;
    CHECK(append(1, 3, text));
    memcpy(text + 198, "\xe6\x8e\xa8", 3); /* 「推」跨过第 200 个字节 */
    CHECK(append(2, 3, text));
    root = message(1, 0);
    CHECK(strlen(text_of(root, 0)) == WASM_LOG_TEXT_BYTES);
    CHECK(strlen(text_of(root, 1)) == WASM_LOG_TEXT_BYTES && !strcmp(text_of(root, 1) + 198, "??"));
    cJSON_Delete(root);
}

static void test_capacity(void)
{
    /* 攒不下的那一行整行不要，已经攒下的不受影响。 */
    s_length = 0;
    int kept = 0;
    static char text[WASM_LOG_TEXT_BYTES + 1];
    memset(text, 'y', WASM_LOG_TEXT_BYTES);
    while (append((uint64_t)kept, 3, text)) ++kept;
    CHECK(kept == 4 && s_length <= sizeof s_lines);
    const size_t full = s_length;
    CHECK(!append(99, 3, text) && s_length == full);
    CHECK(append(100, 3, "short lines still fit") && s_length > full);
    cJSON *root = message(2, 1);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "lines")) == kept + 1);
    cJSON_Delete(root);
    /* 最坏的一行：200 个都要转义成 6 个字节的控制字符，一条消息仍然放得下。 */
    s_length = 0;
    memset(text, 0x01, WASM_LOG_TEXT_BYTES);
    CHECK(!append(1, 3, text) && s_length == 0); /* 1,200 多字节，超过两次发送之间能攒的量 */
    memset(text, '"', WASM_LOG_TEXT_BYTES);
    CHECK(append(1, 3, text) && s_length < sizeof s_lines);
    root = message(4000000000u, 4000000000u);
    CHECK(cJSON_IsObject(root) && cJSON_GetObjectItem(root, "seq")->valuedouble == 4000000000.0);
    cJSON_Delete(root);
    /* 输出缓冲不够：干净地失败。 */
    char small[32];
    CHECK(wasm_log_encode(small, sizeof small, 1, 0, s_lines, s_length) == -1);
}

/* 合同样例（docs/design/device-protocol-samples.json 的 app_log）：设备编码的样子要能通过 Server 的核对规则。 */
static bool sample_shape_ok(const cJSON *payload)
{
    const cJSON *seq = cJSON_GetObjectItem(payload, "seq"), *dropped = cJSON_GetObjectItem(payload, "dropped");
    const cJSON *lines = cJSON_GetObjectItem(payload, "lines"), *line;
    if (!cJSON_IsObject(payload) || cJSON_GetArraySize(payload) != 3 || !cJSON_IsNumber(seq) || seq->valuedouble < 1 ||
        seq->valuedouble != (double)(uint32_t)seq->valuedouble || !cJSON_IsNumber(dropped) || dropped->valuedouble < 0 ||
        dropped->valuedouble != (double)(uint32_t)dropped->valuedouble || !cJSON_IsArray(lines))
        return false;
    cJSON_ArrayForEach(line, lines) {
        const cJSON *t = cJSON_GetObjectItem(line, "t"), *level = cJSON_GetObjectItem(line, "level");
        const cJSON *text = cJSON_GetObjectItem(line, "text");
        if (!cJSON_IsObject(line) || cJSON_GetArraySize(line) != 3 || !cJSON_IsNumber(t) || t->valuedouble < 0 ||
            !cJSON_IsString(level) || !cJSON_IsString(text) || strlen(text->valuestring) > WASM_LOG_TEXT_BYTES * 3)
            return false;
        if (strcmp(level->valuestring, "error") && strcmp(level->valuestring, "warn") && strcmp(level->valuestring, "info"))
            return false;
    }
    return true;
}

static void test_samples(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    static char text[131072];
    const size_t size = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    cJSON *root = cJSON_ParseWithLength(text, size);
    const cJSON *samples = cJSON_GetObjectItem(root, "app_log"), *sample;
    CHECK(cJSON_IsArray(samples) && cJSON_GetArraySize(samples) >= 6);
    cJSON_ArrayForEach(sample, samples) {
        const bool expected = !strcmp(cJSON_GetObjectItem(sample, "expect")->valuestring, "ok");
        const bool ok = sample_shape_ok(cJSON_GetObjectItem(sample, "payload"));
        if (ok != expected) fprintf(stderr, "app_log sample %s\n", cJSON_GetObjectItem(sample, "name")->valuestring);
        CHECK(ok == expected);
    }
    /* 设备按第一个样例的内容编码，结果与样例等价。 */
    const cJSON *first = cJSON_GetObjectItem(cJSON_GetArrayItem(samples, 0), "payload"), *line;
    s_length = 0;
    cJSON_ArrayForEach(line, cJSON_GetObjectItem(first, "lines")) {
        const char *level = cJSON_GetObjectItem(line, "level")->valuestring;
        CHECK(append((uint64_t)cJSON_GetObjectItem(line, "t")->valuedouble, !strcmp(level, "error") ? 1 : !strcmp(level, "warn") ? 2 : 3,
                     cJSON_GetObjectItem(line, "text")->valuestring));
    }
    cJSON *encoded = message((uint32_t)cJSON_GetObjectItem(first, "seq")->valuedouble,
                             (uint32_t)cJSON_GetObjectItem(first, "dropped")->valuedouble);
    CHECK(cJSON_Compare(encoded, first, true));
    cJSON_Delete(encoded);
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    test_lines();
    test_escaping();
    test_capacity();
    if (argc > 1) test_samples(argv[1]);
    printf("wasm log tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
