// SPDX-License-Identifier: Apache-2.0
// WASM 宿主：把编译成 WASM 的业务应用作为业务层运行（darren-space#113）。
// 固件在 app_main 里把 base_wasm_business 交给 base_system_run，应用的更新从此不需要重刷固件。
#pragma once
#include "base_runtime.h"

extern const base_business_t base_wasm_business;

/* 可行性实测：不启动系统层，加载内嵌的样例应用，打印启动前后堆、内存池和任务栈的占用，再测事件调用的耗时。
 * 只在 CONFIG_ESP_BASE_TEST_WASM_BENCH 的测试固件里调用，供 QEMU 与实板运行；不返回。 */
void base_wasm_bench(void);

/* 部署的设备级测试：不启动系统层，按脚本逐个部署预先放进 Flash 的应用包，核对验签、试运行、提交、回退和
 * 重启后的恢复。只在 CONFIG_ESP_BASE_TEST_WASM_DEPLOY 的测试固件里调用；不返回。 */
void base_wasm_deploy_test(void);
