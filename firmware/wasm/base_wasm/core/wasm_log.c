// SPDX-License-Identifier: Apache-2.0
#include "wasm_log.h"

#include <stdio.h>
#include <string.h>

/* 从 text[at] 起是不是一个完整、合法的 UTF-8 多字节字符；是则返回它的字节数，否则 0。 */
static size_t utf8_sequence(const unsigned char *text, size_t at, size_t length)
{
    const unsigned char lead = text[at];
    const size_t more = lead >= 0xC2 && lead <= 0xDF ? 1 : lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;
    if (!more || at + more >= length) return 0; /* 续字节要都在正文里 */
    for (size_t next = 1; next <= more; ++next)
        if ((text[at + next] & 0xC0) != 0x80) return 0;
    return more + 1;
}

typedef struct {
    char *out;
    size_t at, capacity;
    bool full;
} cursor_t;

static void put(cursor_t *cursor, const char *data, size_t bytes)
{
    if (cursor->full || cursor->at + bytes > cursor->capacity) {
        cursor->full = true;
        return;
    }
    memcpy(cursor->out + cursor->at, data, bytes);
    cursor->at += bytes;
}

bool wasm_log_append(char *lines, size_t capacity, size_t *length, uint64_t uptime_ms, int level, const char *text,
                     size_t text_length)
{
    /* 直接写进 lines 的空余部分：不另占一块缓冲，放不下时 *length 不动，写了一半的内容等于没写。 */
    cursor_t cursor = {.out = lines, .at = *length, .capacity = capacity};
    const unsigned char *bytes = (const unsigned char *)text;
    char piece[64];
    if (text_length > WASM_LOG_TEXT_BYTES) text_length = WASM_LOG_TEXT_BYTES;
    put(&cursor, piece, (size_t)snprintf(piece, sizeof piece, "%s{\"t\":%llu,\"level\":\"%s\",\"text\":\"", *length ? "," : "",
                                         (unsigned long long)uptime_ms, level <= 1 ? "error" : level == 2 ? "warn" : "info"));
    for (size_t at = 0; at < text_length && !cursor.full;) {
        const unsigned char c = bytes[at];
        size_t consumed = 1;
        if (c == '"' || c == '\\') {
            piece[0] = '\\';
            piece[1] = (char)c;
            put(&cursor, piece, 2);
        } else if (c < 0x20 || c == 0x7F) {
            put(&cursor, piece, (size_t)snprintf(piece, sizeof piece, "\\u%04x", c));
        } else if (c < 0x80) {
            put(&cursor, text + at, 1);
        } else if ((consumed = utf8_sequence(bytes, at, text_length)) != 0) {
            put(&cursor, text + at, consumed);
        } else {
            put(&cursor, "?", 1); /* 不是合法的 UTF-8，或者被截断在半个字符上 */
            consumed = 1;
        }
        at += consumed;
    }
    put(&cursor, "\"}", 2);
    if (cursor.full) return false;
    *length = cursor.at;
    return true;
}

int wasm_log_encode(char *out, size_t size, uint32_t seq, uint32_t dropped, const char *lines, size_t lines_length)
{
    const int head = snprintf(out, size, "{\"seq\":%u,\"dropped\":%u,\"lines\":[", (unsigned)seq, (unsigned)dropped);
    if (head < 0 || (size_t)head + lines_length + 3 > size) return -1;
    memcpy(out + head, lines, lines_length);
    memcpy(out + head + lines_length, "]}", 3);
    return head + (int)lines_length + 2;
}
