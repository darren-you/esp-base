// SPDX-License-Identifier: Apache-2.0
// 应用日志的远程通道（app/log）：把应用写的日志攒成一条消息的载荷。纯逻辑，不依赖 ESP-IDF。
//
//   {"seq":17,"dropped":0,"lines":[{"t":123456,"level":"info","text":"…"},…]}
//
// seq 是本次启动以来的第几条消息（从 1 起，Server 据此发现中间丢了消息）；dropped 是上一条消息之后因为攒不下
// 而丢掉的行数；t 是开机以来的毫秒数。消息不保留、QoS 0：日志是给人看的，丢了不重发。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_LOG_TEXT_BYTES 200   /* 一行日志正文的上限：超出的截掉 */
#define WASM_LOG_LINES_BYTES 1024 /* 两次发送之间最多攒这么多（已经是 JSON 的行） */
#define WASM_LOG_BYTES 1100       /* 一条 app/log 消息的上限 */

/* 把一行日志接到 lines 后面（lines 是 JSON 数组里的元素，逗号分隔，不以 NUL 结尾）。level 与宿主接口相同：
 * 1 及以下是 error，2 是 warn，其余是 info。正文里的引号、反斜杠和控制字符转义，不是合法 UTF-8 的字节换成 ?。
 * 放不下时返回 false，lines 不变。 */
bool wasm_log_append(char *lines, size_t capacity, size_t *length, uint64_t uptime_ms, int level, const char *text,
                     size_t text_length);

/* 编码一条 app/log 消息。返回长度（out 以 NUL 结尾）；缓冲不足返回 -1。 */
int wasm_log_encode(char *out, size_t size, uint32_t seq, uint32_t dropped, const char *lines, size_t lines_length);
