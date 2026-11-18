// SPDX-License-Identifier: Apache-2.0
// 应用包：一个签名的、可以独立部署的业务应用。纯逻辑，不依赖 ESP-IDF，可在 host 上测试。
//
// 布局（小端）：
//   0    4  魔数 "EBAP"
//   4    2  格式版本，目前是 1
//   6    2  包头长度，固定 32
//   8    4  清单长度
//   12   4  wasm 长度
//   16  16  保留，全 0
//   32      清单（UTF-8 JSON），紧跟 wasm 模块
//           用 0xFF 补齐到 4 KiB 边界
//   …   4096 官方 Secure Boot V2 签名块（espsecure sign-data --version 2），覆盖它之前的全部内容
//
// 包的身份是整个文件（含签名扇区）的 SHA-256。验签在设备上用官方 esp_secure_boot_verify_signature，
// 信任根与固件 OTA 相同；这里只负责包头和清单。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_ABI 1 /* 这个固件提供的宿主接口版本：接口只增不改，每次新增加一 */

#define WASM_PACKAGE_MAGIC "EBAP"
#define WASM_PACKAGE_FORMAT 1
#define WASM_PACKAGE_HEADER_BYTES 32
#define WASM_PACKAGE_SECTOR_BYTES 4096
#define WASM_PACKAGE_MANIFEST_MAX 1024
#define WASM_PACKAGE_NAME_BYTES 24    /* 应用名：1–23 字节的小写字母、数字和下划线，字母开头 */
#define WASM_PACKAGE_VERSION_BYTES 24 /* X.Y.Z+构建号 */
#define WASM_PACKAGE_CHIPS 4
#define WASM_PACKAGE_CHIP_BYTES 16
#define WASM_PACKAGE_MEMORY_MIN 4096u
#define WASM_PACKAGE_MEMORY_MAX 65536u
#define WASM_PACKAGE_STACK_MIN 1024u
#define WASM_PACKAGE_STACK_MAX 8192u

typedef struct {
    char name[WASM_PACKAGE_NAME_BYTES];
    char version[WASM_PACKAGE_VERSION_BYTES];
    uint32_t abi;          /* 应用需要的最低宿主接口版本 */
    uint32_t memory_bytes; /* 运行时内存池的大小，也就是应用的内存上限（线性内存另计） */
    uint32_t stack_bytes;  /* 应用的操作数栈 */
    uint8_t chip_count;    /* 0：不限芯片 */
    char chips[WASM_PACKAGE_CHIPS][WASM_PACKAGE_CHIP_BYTES];
    uint32_t wasm_offset, wasm_bytes; /* wasm 模块在包里的位置 */
    uint32_t signed_bytes;            /* 签名覆盖的长度：包头、清单、wasm 补齐到 4 KiB */
    uint32_t total_bytes;             /* 整个文件：signed_bytes 加一个签名扇区 */
} wasm_package_t;

/* 解析包头和清单。data 是包的开头，至少包含包头和清单（读进前 4 KiB 即可）；slot_bytes 是存放它的槽的大小。
 * 成功返回 NULL；失败返回原因（package_invalid、package_too_large、manifest_invalid），可以直接作为上报的 error。 */
const char *wasm_package_parse(const uint8_t *data, size_t available, size_t slot_bytes, wasm_package_t *out);

/* 这个固件能不能运行它。chip 是芯片型号（esp32c3、esp32）；memory_max 是这个芯片允许的内存池上限。
 * 可以返回 NULL，或 abi_mismatch、chip_mismatch、memory_too_large。 */
const char *wasm_package_accept(const wasm_package_t *package, const char *chip, uint32_t memory_max);
