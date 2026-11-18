// SPDX-License-Identifier: Apache-2.0
// host 测试：应用包的包头与清单解析，以及打包工具产出的样例包（tests/apps/hello.ebapp）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_package.h"

static int s_failures, s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { ++s_failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

#define SLOT 32768u
#define MANIFEST "{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"

static uint8_t s_buffer[SLOT];

/* 在 s_buffer 里造一个包：包头、清单，wasm 的内容对解析无关紧要。返回已写入的长度。 */
static size_t make(const char *manifest, uint32_t wasm_bytes)
{
    const uint32_t manifest_bytes = (uint32_t)strlen(manifest);
    memset(s_buffer, 0, sizeof s_buffer);
    memcpy(s_buffer, "EBAP", 4);
    s_buffer[4] = 1;
    s_buffer[6] = 32;
    for (int index = 0; index < 4; ++index) {
        s_buffer[8 + index] = (uint8_t)(manifest_bytes >> (8 * index));
        s_buffer[12 + index] = (uint8_t)(wasm_bytes >> (8 * index));
    }
    memcpy(s_buffer + 32, manifest, manifest_bytes);
    return 32 + manifest_bytes;
}

static const char *parse_manifest(const char *manifest, wasm_package_t *out)
{
    const size_t available = make(manifest, 1550);
    return wasm_package_parse(s_buffer, available, SLOT, out);
}

static bool rejected(const char *manifest)
{
    wasm_package_t package;
    const char *error = parse_manifest(manifest, &package);
    return error && !strcmp(error, "manifest_invalid") && package.name[0] == '\0';
}

static void test_header(void)
{
    wasm_package_t package;
    size_t available = make(MANIFEST, 1550);
    CHECK(wasm_package_parse(s_buffer, available, SLOT, &package) == NULL);
    CHECK(!strcmp(package.name, "hello") && !strcmp(package.version, "0.1.0+7") && package.abi == 1);
    CHECK(package.memory_bytes == 16384 && package.stack_bytes == 2048 && package.chip_count == 0);
    CHECK(package.wasm_offset == 32 + strlen(MANIFEST) && package.wasm_bytes == 1550);
    /* 32 + 清单 + 1550 补齐到 4 KiB，再加一个签名扇区。 */
    CHECK(package.signed_bytes == 4096 && package.total_bytes == 8192);

    /* 只给了包头：清单还没读到。 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, 32, SLOT, &package), "package_invalid"));
    CHECK(!strcmp(wasm_package_parse(s_buffer, 31, SLOT, &package), "package_invalid"));
    CHECK(!strcmp(wasm_package_parse(NULL, 0, SLOT, &package), "package_invalid"));

    make(MANIFEST, 1550);
    s_buffer[0] = 'X';
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_invalid"));
    make(MANIFEST, 1550);
    s_buffer[4] = 2; /* 不认识的格式版本 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_invalid"));
    make(MANIFEST, 1550);
    s_buffer[6] = 36; /* 包头长度不对 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_invalid"));
    make(MANIFEST, 1550);
    s_buffer[20] = 1; /* 保留字节必须是 0 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_invalid"));
    make(MANIFEST, 4); /* 连 wasm 的魔数和版本都放不下 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_invalid"));
    make(MANIFEST, 1550);
    memset(s_buffer + 8, 0, 4); /* 没有清单 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_invalid"));
    make(MANIFEST, 1550);
    s_buffer[9] = 0x10; /* 清单 4 KiB 以上 */
    CHECK(!strcmp(wasm_package_parse(s_buffer, sizeof s_buffer, SLOT, &package), "package_invalid"));
}

static void test_size(void)
{
    wasm_package_t package;
    /* 槽 32 KiB：内容最多 28 KiB，留一个扇区给签名。 */
    const uint32_t fits = 28672 - 32 - (uint32_t)strlen(MANIFEST);
    size_t available = make(MANIFEST, fits);
    CHECK(wasm_package_parse(s_buffer, available, SLOT, &package) == NULL);
    CHECK(package.signed_bytes == 28672 && package.total_bytes == SLOT);
    available = make(MANIFEST, fits + 1);
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_too_large"));
    /* 长度来自包本身：极大的值不能让加法溢出后蒙混过关。 */
    available = make(MANIFEST, 0xFFFFFFF0u);
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_too_large"));
    available = make(MANIFEST, 0xFFFFF000u);
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, SLOT, &package), "package_too_large"));
    available = make(MANIFEST, 1550);
    CHECK(!strcmp(wasm_package_parse(s_buffer, available, 4096, &package), "package_too_large"));
}

static void test_manifest(void)
{
    wasm_package_t package;
    CHECK(parse_manifest("{\"name\":\"hxh_lift\",\"version\":\"1.0.0+12\",\"abi\":1,\"memory\":8192,\"stack\":1024,"
                         "\"chips\":[\"esp32c3\",\"esp32\"]}", &package) == NULL);
    CHECK(package.chip_count == 2 && !strcmp(package.chips[0], "esp32c3") && !strcmp(package.chips[1], "esp32"));

    /* 名称：小写字母开头，1–23 个字符。 */
    CHECK(rejected("{\"name\":\"\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"Hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"1app\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"a-b\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"abcdefghijklmnopqrstuvwx\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":7,\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    /* 版本：X.Y.Z+构建号。 */
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.01.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7x\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"v0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    /* 数值的范围和类型。 */
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":0,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1.5,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":\"1\",\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":4095,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":65537,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":-1,\"stack\":2048}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":1023}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":8193}"));
    /* 芯片列表。 */
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,\"chips\":[]}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,\"chips\":\"esp32c3\"}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,\"chips\":[\"ESP32\"]}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,"
                   "\"chips\":[\"a\",\"b\",\"c\",\"d\",\"e\"]}"));
    /* 缺键、多余的键、重复的键、不是对象、不是 JSON。 */
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384}"));
    CHECK(rejected("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,\"extra\":1}"));
    CHECK(rejected("{\"name\":\"hello\",\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048}"));
    CHECK(rejected("[1,2,3]"));
    CHECK(rejected("{\"name\":\"hello\""));
    CHECK(rejected("not json"));
}

static void test_accept(void)
{
    wasm_package_t package;
    CHECK(parse_manifest("{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,"
                         "\"chips\":[\"esp32c3\"]}", &package) == NULL);
    CHECK(wasm_package_accept(&package, "esp32c3", 32768) == NULL);
    CHECK(!strcmp(wasm_package_accept(&package, "esp32", 32768), "chip_mismatch"));
    CHECK(!strcmp(wasm_package_accept(&package, "esp32c3", 8192), "memory_too_large"));
    package.abi = WASM_ABI + 1; /* 应用要的接口比固件新 */
    CHECK(!strcmp(wasm_package_accept(&package, "esp32c3", 32768), "abi_mismatch"));
    CHECK(parse_manifest(MANIFEST, &package) == NULL);
    CHECK(wasm_package_accept(&package, "esp32", 32768) == NULL); /* 没有 chips：不限芯片 */
}

#define WITH_PINS(pins) "{\"name\":\"hello\",\"version\":\"0.1.0+7\",\"abi\":1,\"memory\":16384,\"stack\":2048,\"pins\":" pins "}"

static const char *accept_pins(const char *manifest, const char *chip)
{
    wasm_package_t package;
    if (parse_manifest(manifest, &package)) return "unparsed";
    const char *error = wasm_package_accept(&package, chip, 32768);
    return error ? error : "ok";
}

static void test_pins(void)
{
    wasm_package_t package;
    CHECK(parse_manifest(WITH_PINS("{\"4\":\"out0\",\"5\":\"out1\",\"0\":\"in\",\"10\":\"in_pullup\",\"21\":\"in_pulldown\","
                                   "\"7\":\"pwm\"}"), &package) == NULL);
    CHECK(package.pin_count == 6 && package.pins[0].pin == 4 && package.pins[0].mode == WASM_PIN_OUT0);
    CHECK(package.pins[1].pin == 5 && package.pins[1].mode == WASM_PIN_OUT1 && package.pins[2].mode == WASM_PIN_IN);
    CHECK(package.pins[3].pin == 10 && package.pins[3].mode == WASM_PIN_IN_PULLUP);
    CHECK(package.pins[4].pin == 21 && package.pins[4].mode == WASM_PIN_IN_PULLDOWN && package.pins[5].mode == WASM_PIN_PWM);
    CHECK(wasm_pin_mode(package.pins, package.pin_count, 7) == WASM_PIN_PWM && wasm_pin_mode(package.pins, package.pin_count, 8) < 0);
    CHECK(wasm_package_accept(&package, "esp32c3", 32768) == NULL);
    CHECK(parse_manifest(MANIFEST, &package) == NULL && package.pin_count == 0); /* 没有 pins：不用引脚 */

    /* 写法：对象；键是不带前导零的十进制引脚号；值是认识的用途；不重复；最多 8 个。 */
    CHECK(rejected(WITH_PINS("{}")) && rejected(WITH_PINS("[4]")) && rejected(WITH_PINS("\"4\"")));
    CHECK(rejected(WITH_PINS("{\"04\":\"out0\"}")) && rejected(WITH_PINS("{\"4a\":\"out0\"}")));
    CHECK(rejected(WITH_PINS("{\"100\":\"out0\"}")) && rejected(WITH_PINS("{\"\":\"out0\"}")));
    CHECK(rejected(WITH_PINS("{\"-1\":\"out0\"}")) && rejected(WITH_PINS("{\"4\":\"out\"}")));
    CHECK(rejected(WITH_PINS("{\"4\":1}")) && rejected(WITH_PINS("{\"4\":\"OUT0\"}")));
    CHECK(rejected(WITH_PINS("{\"4\":\"out0\",\"4\":\"out1\"}")));
    CHECK(rejected(WITH_PINS("{\"0\":\"in\",\"1\":\"in\",\"2\":\"in\",\"3\":\"in\",\"4\":\"in\",\"5\":\"in\",\"6\":\"in\","
                             "\"7\":\"in\",\"8\":\"in\"}")));

    /* ESP32-C3：GPIO0–10、20、21 可用；11–17 接 Flash，18／19 是 USB。 */
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"0\":\"out0\",\"10\":\"pwm\",\"20\":\"in\",\"21\":\"out1\"}"), "esp32c3"), "ok"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"11\":\"in\"}"), "esp32c3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"17\":\"out0\"}"), "esp32c3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"18\":\"in\"}"), "esp32c3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"19\":\"out0\"}"), "esp32c3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"22\":\"out0\"}"), "esp32c3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"4\":\"out0\",\"12\":\"out0\"}"), "esp32c3"), "pin_denied")); /* 有一个不行就不装 */
    /* ESP32：6–11 接 Flash，1／3 是 UART0，20、24、28–31 不存在；34–39 只能作不带上下拉的输入。 */
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"0\":\"in\",\"2\":\"out0\",\"4\":\"pwm\",\"33\":\"out1\",\"34\":\"in\",\"39\":\"in\"}"), "esp32"), "ok"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"6\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"11\":\"out0\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"1\":\"out0\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"3\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"20\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"24\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"28\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"31\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"40\":\"in\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"34\":\"out0\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"35\":\"pwm\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"36\":\"in_pullup\"}"), "esp32"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"39\":\"in_pulldown\"}"), "esp32"), "pin_denied"));
    /* 每个 PWM 引脚占一个 LEDC 通道：ESP32-C3 有 6 个，ESP32 有 8 个。 */
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"0\":\"pwm\",\"1\":\"pwm\",\"2\":\"pwm\",\"3\":\"pwm\",\"4\":\"pwm\",\"5\":\"pwm\",\"6\":\"out0\","
                                        "\"7\":\"in\"}"), "esp32c3"), "ok"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"0\":\"pwm\",\"1\":\"pwm\",\"2\":\"pwm\",\"3\":\"pwm\",\"4\":\"pwm\",\"5\":\"pwm\",\"6\":\"pwm\"}"),
                              "esp32c3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"2\":\"pwm\",\"4\":\"pwm\",\"5\":\"pwm\",\"12\":\"pwm\",\"13\":\"pwm\",\"14\":\"pwm\",\"15\":\"pwm\","
                                        "\"16\":\"pwm\"}"), "esp32"), "ok"));
    /* 不认识的芯片：有引脚的应用不装，不用引脚的照常。 */
    CHECK(!strcmp(accept_pins(WITH_PINS("{\"4\":\"in\"}"), "esp32s3"), "pin_denied"));
    CHECK(!strcmp(accept_pins(MANIFEST, "esp32s3"), "ok"));
}

/* 打包工具（tools/esp_base.py app）产出的样例包：工具与设备对格式的理解一致。 */
static void test_sample(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    const size_t size = fread(s_buffer, 1, sizeof s_buffer, file);
    fclose(file);
    wasm_package_t package;
    CHECK(wasm_package_parse(s_buffer, size, SLOT, &package) == NULL);
    CHECK(!strcmp(package.name, "hello") && !strcmp(package.version, "0.1.0+0") && package.abi == 1);
    CHECK(package.total_bytes == size); /* 样例是签过名的完整文件 */
    CHECK(!memcmp(s_buffer + package.wasm_offset, "\0asm\1\0\0\0", 8));
    /* 签名块在 4 KiB 边界上，以官方的魔数 0xE7 开头；内容和它之间用 0xFF 补齐。 */
    CHECK(s_buffer[package.signed_bytes] == 0xE7);
    bool padded = true;
    for (uint32_t at = package.wasm_offset + package.wasm_bytes; at < package.signed_bytes; ++at) padded = padded && s_buffer[at] == 0xFF;
    CHECK(padded);
}

int main(int argc, char **argv)
{
    test_header();
    test_size();
    test_manifest();
    test_accept();
    test_pins();
    if (argc > 1) test_sample(argv[1]);
    printf("wasm package tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
