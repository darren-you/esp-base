// SPDX-License-Identifier: Apache-2.0
// 强制包含进 WAMR 的每个源文件（见 firmware/CMakeLists.txt）：WAMR 2.4.6 的 ESP-IDF 端口用到了这些声明却没有
// 包含对应头文件，ESP-IDF 6 的 C 库不再顺带提供。只有它的 WASI 文件访问用得到，本固件不启用 WASI。
#ifndef __ASSEMBLER__
#include <sys/stat.h>
int renameat(int, const char *, int, const char *);
#endif
