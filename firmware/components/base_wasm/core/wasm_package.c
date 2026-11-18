// SPDX-License-Identifier: Apache-2.0
#include "wasm_package.h"

#include <string.h>

#include "cJSON.h"

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool lower_or_digit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* 应用名：小写字母开头，之后是小写字母、数字或下划线。 */
static bool copy_name(char *out, size_t size, const cJSON *item)
{
    if (!cJSON_IsString(item)) return false;
    const char *text = item->valuestring;
    const size_t length = strlen(text);
    if (length == 0 || length >= size || text[0] < 'a' || text[0] > 'z') return false;
    for (size_t index = 1; index < length; ++index)
        if (!lower_or_digit(text[index]) && text[index] != '_') return false;
    memcpy(out, text, length + 1);
    return true;
}

/* 版本：X.Y.Z+构建号，四段都是不带前导零的十进制数。 */
static bool copy_version(char *out, size_t size, const cJSON *item)
{
    if (!cJSON_IsString(item)) return false;
    const char *text = item->valuestring, *p = text;
    const size_t length = strlen(text);
    if (length == 0 || length >= size) return false;
    static const char separators[] = {'.', '.', '+', '\0'};
    for (int part = 0; part < 4; ++part) {
        const char *digits = p;
        while (*p >= '0' && *p <= '9') ++p;
        if (p == digits || (p - digits > 1 && *digits == '0') || *p != separators[part]) return false;
        if (*p) ++p;
    }
    memcpy(out, text, length + 1);
    return true;
}

static bool read_uint(uint32_t *out, const cJSON *item, uint32_t minimum, uint32_t maximum)
{
    if (!cJSON_IsNumber(item)) return false;
    const double value = item->valuedouble;
    if (value < (double)minimum || value > (double)maximum || value != (double)(uint32_t)value) return false;
    *out = (uint32_t)value;
    return true;
}

static bool read_chips(wasm_package_t *out, const cJSON *item)
{
    if (!item) return true; /* 不限芯片 */
    if (!cJSON_IsArray(item)) return false;
    const int count = cJSON_GetArraySize(item);
    if (count < 1 || count > WASM_PACKAGE_CHIPS) return false;
    for (int index = 0; index < count; ++index) {
        const cJSON *chip = cJSON_GetArrayItem(item, index);
        if (!cJSON_IsString(chip)) return false;
        const size_t length = strlen(chip->valuestring);
        if (length == 0 || length >= WASM_PACKAGE_CHIP_BYTES) return false;
        for (size_t at = 0; at < length; ++at)
            if (!lower_or_digit(chip->valuestring[at])) return false;
        memcpy(out->chips[index], chip->valuestring, length + 1);
    }
    out->chip_count = (uint8_t)count;
    return true;
}

static bool read_pins(wasm_package_t *out, const cJSON *item)
{
    static const char *const modes[] = {"in", "in_pullup", "in_pulldown", "out0", "out1", "pwm"};
    if (!item) return true; /* 不用引脚 */
    if (!cJSON_IsObject(item)) return false;
    const int count = cJSON_GetArraySize(item);
    if (count < 1 || count > WASM_PACKAGE_PINS) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        /* 键是引脚号：十进制，不带前导零。 */
        const char *key = entry->string;
        const size_t digits = key ? strlen(key) : 0;
        if (digits < 1 || digits > 2 || (digits == 2 && key[0] == '0') || !cJSON_IsString(entry)) return false;
        unsigned pin = 0;
        for (size_t at = 0; at < digits; ++at) {
            if (key[at] < '0' || key[at] > '9') return false;
            pin = pin * 10 + (unsigned)(key[at] - '0');
        }
        int mode = -1;
        for (int index = 0; index < 6 && mode < 0; ++index)
            if (!strcmp(entry->valuestring, modes[index])) mode = index;
        if (mode < 0 || wasm_pin_mode(out->pins, out->pin_count, pin) >= 0) return false; /* 不认识的用途，或重复的引脚 */
        out->pins[out->pin_count++] = (wasm_pin_t){.pin = (uint8_t)pin, .mode = (uint8_t)mode};
    }
    return true;
}

static bool parse_manifest(const char *text, size_t length, wasm_package_t *out)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    bool ok = cJSON_IsObject(root);
    int known = 0;
    if (ok) {
        const cJSON *chips = cJSON_GetObjectItemCaseSensitive(root, "chips");
        const cJSON *pins = cJSON_GetObjectItemCaseSensitive(root, "pins");
        ok = copy_name(out->name, sizeof out->name, cJSON_GetObjectItemCaseSensitive(root, "name")) &&
             copy_version(out->version, sizeof out->version, cJSON_GetObjectItemCaseSensitive(root, "version")) &&
             read_uint(&out->abi, cJSON_GetObjectItemCaseSensitive(root, "abi"), 1, 65535) &&
             read_uint(&out->memory_bytes, cJSON_GetObjectItemCaseSensitive(root, "memory"), WASM_PACKAGE_MEMORY_MIN,
                       WASM_PACKAGE_MEMORY_MAX) &&
             read_uint(&out->stack_bytes, cJSON_GetObjectItemCaseSensitive(root, "stack"), WASM_PACKAGE_STACK_MIN,
                       WASM_PACKAGE_STACK_MAX) &&
             read_chips(out, chips) && read_pins(out, pins);
        known = 5 + (chips ? 1 : 0) + (pins ? 1 : 0);
    }
    /* 不认识的键、重复的键都不接受：清单是合同，不是备注栏。 */
    ok = ok && cJSON_GetArraySize(root) == known;
    cJSON_Delete(root);
    return ok;
}

const char *wasm_package_parse(const uint8_t *data, size_t available, size_t slot_bytes, wasm_package_t *out)
{
    memset(out, 0, sizeof *out);
    if (!data || available < WASM_PACKAGE_HEADER_BYTES || memcmp(data, WASM_PACKAGE_MAGIC, 4) != 0) return "package_invalid";
    const uint32_t format = (uint32_t)data[4] | (uint32_t)data[5] << 8;
    const uint32_t header_bytes = (uint32_t)data[6] | (uint32_t)data[7] << 8;
    const uint32_t manifest_bytes = read_u32(data + 8), wasm_bytes = read_u32(data + 12);
    if (format != WASM_PACKAGE_FORMAT || header_bytes != WASM_PACKAGE_HEADER_BYTES) return "package_invalid";
    for (int index = 16; index < WASM_PACKAGE_HEADER_BYTES; ++index)
        if (data[index] != 0) return "package_invalid";
    if (manifest_bytes == 0 || manifest_bytes > WASM_PACKAGE_MANIFEST_MAX || wasm_bytes < 8) return "package_invalid";
    /* 先比大小再做加法：长度来自包本身，不能让它溢出。 */
    if (slot_bytes < WASM_PACKAGE_SECTOR_BYTES * 2 || wasm_bytes > slot_bytes) return "package_too_large";
    const uint32_t content = WASM_PACKAGE_HEADER_BYTES + manifest_bytes + wasm_bytes;
    const uint32_t padded = (content + WASM_PACKAGE_SECTOR_BYTES - 1) & ~(uint32_t)(WASM_PACKAGE_SECTOR_BYTES - 1);
    if (padded + WASM_PACKAGE_SECTOR_BYTES > slot_bytes) return "package_too_large";
    if (available < WASM_PACKAGE_HEADER_BYTES + manifest_bytes) return "package_invalid";
    if (!parse_manifest((const char *)data + WASM_PACKAGE_HEADER_BYTES, manifest_bytes, out)) {
        memset(out, 0, sizeof *out);
        return "manifest_invalid";
    }
    out->wasm_offset = WASM_PACKAGE_HEADER_BYTES + manifest_bytes;
    out->wasm_bytes = wasm_bytes;
    out->signed_bytes = padded;
    out->total_bytes = padded + WASM_PACKAGE_SECTOR_BYTES;
    return NULL;
}

const char *wasm_package_accept(const wasm_package_t *package, const char *chip, uint32_t memory_max)
{
    if (package->abi > WASM_ABI) return "abi_mismatch";
    if (package->chip_count) {
        bool listed = false;
        for (int index = 0; index < package->chip_count && !listed; ++index) listed = !strcmp(package->chips[index], chip);
        if (!listed) return "chip_mismatch";
    }
    if (package->memory_bytes > memory_max) return "memory_too_large";
    unsigned pwm = 0;
    for (int index = 0; index < package->pin_count; ++index) {
        const wasm_pin_mode_t mode = (wasm_pin_mode_t)package->pins[index].mode;
        if (!wasm_pin_allowed(chip, package->pins[index].pin, mode)) return "pin_denied";
        pwm += mode == WASM_PIN_PWM;
    }
    /* 每个 PWM 引脚占一个 LEDC 通道：ESP32-C3 有 6 个，ESP32 的低速组有 8 个。 */
    if (pwm > (!strcmp(chip, "esp32c3") ? 6u : 8u)) return "pin_denied";
    return NULL;
}

int wasm_pin_mode(const wasm_pin_t *pins, unsigned count, unsigned pin)
{
    for (unsigned index = 0; index < count; ++index)
        if (pins[index].pin == pin) return pins[index].mode;
    return -1;
}

bool wasm_pin_allowed(const char *chip, unsigned pin, wasm_pin_mode_t mode)
{
    if (!strcmp(chip, "esp32c3")) {
        /* GPIO0–21。11–17 接内置 Flash，18／19 是 USB Serial/JTAG：占用后设备就不能再经 USB 刷写和看日志。 */
        return pin <= 21 && !(pin >= 11 && pin <= 19);
    }
    if (!strcmp(chip, "esp32")) {
        /* 没有的引脚：20、24、28–31。6–11 接 Flash，1／3 是下载和日志用的 UART0。 */
        if (pin > 39 || pin == 20 || pin == 24 || (pin >= 28 && pin <= 31)) return false;
        if ((pin >= 6 && pin <= 11) || pin == 1 || pin == 3) return false;
        /* 34–39 只能输入，也没有内部上下拉。 */
        return pin < 34 || mode == WASM_PIN_IN;
    }
    return false; /* 不认识的芯片：不放行任何引脚 */
}
