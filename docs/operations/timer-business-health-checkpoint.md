# 定时事件业务失败与试运行健康检查点

日期：2026-10-04。

## 原故障与最小修正

Container ABI 2 将 `on_event` 的负返回值定义为业务失败。定时器通过同一 `on_event` 投递；正常完成的调用可以同时返回 `ECONTAINER_RUNTIME_OK` 和业务结果 `-7`。Base 外部事件已在 `business_trial()` 中饱和累计失败次数并撤销代表事件，但原定时分支丢弃了这个业务结果。

真实签名定时 guest 已复现：先取得代表事件，再触发一次返回 `-7` 的定时回调，产品 runtime 仍为 RUNNING，原代码却保留代表序号 1／失败数 0；原健康依据随后完成确认及两次持久写入。修正只在定时调用正常完成且业务结果为负时，持现有事件锁执行相同的试运行失败记账。它继续运行 guest，不产生 VM trap，不改变已确认产品的统计策略或引入新的持久状态。

生产改动仅 `firmware/integrations/container_binding/esp_base_container_product.c` 十行，源文件 SHA-256 为 `e1c0f1d9716c148887a867911705e5d6bd8827bcb20780920034970da79c36c8`。Container、WAMR、平台预算、ABI、分区布局、签名与信任策略均保持原输入。

## 软件输入与真实执行

| 输入 | 精确版本 |
| --- | --- |
| 原 Base | `7833e021e3db06cfb4d3ed7834b51ed7d7110d07` |
| Container | `15b74a2172a6ddc1f0ad2748c4c060dfa065c80b` |
| WAMR | `74fd95ccbdc417c3816e04f3308eea8a5473ed34` |
| wasi-sdk | `33.0`，macOS ARM64 |
| OTA／FRP／MQTT 测试头与源码 | 正式 manager 解析后的原组件字节，分别对应 Base 锁中的 `bf11916a`／`989cc876`／`6443b71d` |

`tools/container_product_timer_business_test.py` 使用既有构包器和临时 RSA-3072 测试键，为同一真实 guest 编译并签署 `-7`、`0`、`3` 三个返回值变体。正式产品 owner 重新验签、启动真正 WAMR 实例；guest 的查询事件读取回调计数，证明定时回调已执行一次。健康窗口 wrapper 直接调用实际 `esp_base_protocol.c` 的 `observe_business_trial_window`，没有重写或镜像健康策略。

本轮关键回归覆盖：

- 试运行已有代表事件后，单次 `-7` 定时回调将失败数增至 1、代表序号清零；runtime 保持 RUNNING、实例及事件入口保持活动。
- 以失败前序号／失败数调用正式确认入口必须拒绝，且无持久写入。
- 原健康窗口在下一次采样清空；新的合法代表事件按既有规则重开完整 30 秒窗口，29 秒仍拒绝，30 秒并且真实 owner 空闲后可确认。
- `0` 与 `3` 两个正常返回保持原代表事件与失败数 0，可按原确认原语成功提交。
- 正式生命周期入口继续运行原期限、异步取消、签名／包身份、冷启动恢复、百次重装和 Darwin 普通资源回归。

窗口测试的 Wi-Fi／时间／MQTT ready 为显式宿主事实替身，采样时刻由测试提供；实际定时回调、Base owner、槽事务、验签、WAMR 和健康策略均为生产实现。这不证明设备真实联网连续 30 秒。

## 实际退出与失败保留

第一次运行触发上述真实错误确认及断言，宿主子进程未正常退出；本次测试父进程被显式终止后入口退出 137。随后采用明确错误诊断及 `exit(1)`，第二轮在定时用例之前遇到 `deadline.pkg` 写入 ENOSPC，入口退出 1。两者分别保留，不作自然行为红资格。

空间恢复后的第三轮，生产仍为原 Base 字节：真实回调计数 1，代表序号 1／失败数 0，错误确认产生两次写入；C 二进制自然退出 1，Python 和正式 shell 同为 1。这是本轮修复前行为红。

修正后的四条正式入口均已实际完成：

| 目标／入口 | 实际退出 | 本轮耗时 |
| --- | --- | --- |
| ESP32-C3 完整生命周期 | 0 | 26.869 秒 |
| ESP32 完整生命周期 | 0 | 31.224 秒 |
| ESP32-C3 完整 host | 0 | 30.543 秒 |
| ESP32 完整 host | 0 | 31.594 秒 |

两个生命周期均覆盖真实 `-7`／`0`／`3` 回调及原有生命周期／资源门；两目标普通百次资源观测的 10／50／100 轮 malloc 均为 389200 字节，各自虚拟内存与 region 数亦保持相同。这只证明本轮 Darwin 宿主资源回归，不代表 MCU 容量通过。各次命令、环境、输入 SHA、完整日志与日志摘要由仓外最终候选清单独立保存；原失败结果保持不变。

公开复验入口为仓库根的 `bash firmware/tests/run_container_lifecycle_test.sh <精确干净Container> <精确干净WAMR> <wasi-sdk-33根>`；ESP32 设置 `ESP_BASE_TEST_TARGET=esp32`。两目标完整 host 使用 `bash firmware/tests/run_host_tests.sh`。临时构包需要 `TEST_PYTHON` 所指 Python 具备 `cryptography`；所有执行前清除 `ESP_TEST*` 设备测试变量。

## 验收边界

Base 生命周期、产品、协议和账本测试按原入口启用 ASan／UBSan；原 app_main TU 与 Container／WAMR 库仍按原入口普通编译，不能声称整个运行库都启用了 sanitizer。Darwin 普通百次资源测试与 sanitizer 执行独立。

本轮验证仅在仓外执行，不写 UART／USB、设备、eFuse、生产凭据或生产网络。MCU 定时调度、实板 RAM／栈、真实掉电及五能力容量仍须各自取得实际结果，不能由宿主回归替代。

## Root 官方 SDK 构建与成本复核

同一生产文件在锁定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 下通过 C3／ESP32 两次完整 SDK 构建与官方验签。仅使用既有独立软件测试键；两目标 sdkconfig、精确依赖锁、预算、签名方案、正式分区几何及 2752 个解析组件文件的字节／执行位与原 `7833e021` 组合相同，实际编译命令消费上述生产 SHA。

| 目标 | 完整 signed bin | 当前 app 槽余量 | `.flash.text` 变化 | 产品线程局部 frame |
| --- | ---: | ---: | ---: | ---: |
| ESP32-C3 | 1,183,744 B | 61,440 B | +92 B | 1,296 B，保持 |
| ESP32 | 1,114,100 B | 65,548 B | +76 B | 1,280 B，保持 |

两目标除代码段外，其余分配段尺寸及内部堆起点保持。signed bin 尺寸由原 padding 吸收该代码变化，不能称为 Flash 或 RAM 节省；局部 frame 不代表完整调用链栈峰值。未运行新源码的设备、QEMU、联网或动态 RAM 采样，48 KiB 总门及原实板最低堆差额仍开放。
