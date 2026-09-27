# Classic 入口期限精确锁的双目标签名 QEMU 检查点

2026-09-27，以 `esp-base@325ee511b6e48e391ec436fe1d35c6f377d34002` 的独立归档，在 `mac-work-1` 对新 Container／WAMR 锁复验签名 guest 的产品卸载。ESP32 与 C3 均从预置的真实签名 ABI 2 counter 包启动运行 guest，随后由隔离测试任务调用正式 Base API 完成 `stop_confirmed → product_uninstall → product_boot → release`；同片 Flash 冷启动仍为无包绑定。本轮不写实体设备。

## 精确输入

原始证据目录是 `mac-work-1:/private/tmp/esp-base-classic-deadline-qemu-20260927/`，下文记为 `$RUN`。`git archive` 的 gzip SHA-256 为 `75a02d5cd1c96909ba1f95f3bc572d0b27e07bd9ec856f255723b10061032456`。固定 ESP-IDF 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`；双目标主固件锁定 Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 与 OTA `f4fb0b4f3fa7b384edf540bac626314418156d22`。`compile_commands.json` 确认两目标真实编译的 `wasm_interp_classic.c` 和 Container `runtime.c` 都携带 `WASM_ENABLE_CLASSIC_WALL_CLOCK_LIMIT=1`。QEMU 为 Espressif `9.2.2 (esp_develop_9.2.2_20260417)`。

普通 Base 没有设备产品操作消费者，链接器会丢弃卸载入口。仅在归档副本运行[测试变体准备脚本](../../tools/prepare_qemu_product_uninstall_probe.py)，在 `READY` 后创建 16 KiB FreeRTOS 测试任务，持唯一 storage claim 调用正式 API；没有改 Container、WAMR、NVS 或 OTA 实现。两个目标的原 `esp_base_main.c` SHA-256 都是 `41523e4ba841fcf5487b414b5fb9d574c1b18823e28e956fe5ce383e7e921a3b`，变体分别为 C3 `b5ef62f3770682341b2ee0486f55141a77b422f4651a78228952873630708fc1`、ESP32 `d426b9b6a14c451763fddd04af0d6964359e8323974deef67962b839bfb31049`，与[先前测试方法](product-uninstall-qemu-checkpoint.md)相同。测试任务输入为本次包 SHA-256 与 ECS2 初始序号 6。

| 目标 | 隔离布局与签名输入 | 本轮测试变体签名 app |
| --- | --- | --- |
| C3 | 候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`；仅归档副本将 C3 OTA policy 改为 `ota_1@0x140000`、槽 `0x120000`；sdkconfig SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`；既有仓外 RSA v2 测试键 | `0x111000` B，SHA-256 `c2c4f1e80eeee558b1e076426c9851144e4c1db6ac8e45975be2488471c66128`，每槽余 `0xf000`；官方 RSA v2 验签成功 |
| ESP32 | **当前正式 CSV** SHA-256 `f3f29e52f2ed0ccb3fbb3faf9e3d978d359aa9a958c2b3a6120399af70f11b73`，`base_store@0x3ea000/0x16000`；sdkconfig SHA-256 `943406f3351f9786567a684d03c152e59e1a175fd819d44eea1f9e8e56e303b3`；既有仓外 ECDSA v1 测试键 | `0x10fff4` B，SHA-256 `0708afadafa08555c297e1a9dda90cde939f1650e53ea98a4c0c1260f96ebd50`，每槽余 `0x1000c`；官方 app 与分区表 ECDSA v1 验签成功 |

两包各 10,240 B：C3 SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`，ESP32 SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`。`$RUN/<目标>/seed.c` 对本轮签名 app 使用**本轮新锁的** Container `slots.c`、包 validator 与 Wasm 校验源码，按 `initialize → reserve → write_and_prepare → begin_trial → mark_healthy → confirm → reconcile` 生成 `CONFIRMED` 的 ECS2 sequence 6 和包区；这只制造前置状态，不是设备安装 API。固定 SDK 的 NVS V2 generator 把 blob 写入各自 `base_store`，`$RUN/assemble_flash.py` 按本轮 `flasher_args.json` 拼接 bootloader、分区表、otadata、签名 app、包区和 NVS。初始合成 Flash SHA-256 为 C3 `0b850c2072d2f435a66e59e071c9c4b32618cd1a5d546db9447eddfd5d986d73`、ESP32 `3efb916b5405155481025d3acb7e5fcd44e0790ae99f82de53705ce0a010feb9`。逐段偏移与摘要见 `$RUN/<目标>/flash-receipt.json`。

## QEMU 执行及持久读回

执行脚本、GDB 命令、原始日志保存在 `$RUN/<目标>/`，没有覆盖先前 `e536b3d` 的证据。C3 每次只在 QEMU 未模拟的 `adc2_init_code_calibration` 入口由 GDB 将 PC 设为返回地址，签名 app 字节不变；USB Serial/JTAG 的产品日志不在 QEMU UART0，C3 状态以真实任务后的 GDB 变量为证。ESP32 只用 UART 观察，无 GDB 跳过。

| 目标 | 首次启动、卸载 | 同片冷启动 |
| --- | --- | --- |
| C3 | GDB：`stage=4`、`stop=1`、`uninstall=0 (COMPLETE)`、同 boot `reopen=2 (EMPTY)`、`release=1`；`thread_joinable=false`、`instance_active=false`、`native_reclaimed=true`、claim 清零。`uninstall-gdb.log` SHA-256 `3dc92ae648af87b4648eef3a101e8ea25bce8580304bb416a9dca71d64166c7f`。 | `s_product.result=2 (EMPTY)`，无可 join 线程、无活动实例、claim 清零；`reboot-gdb.log` SHA-256 `4b1939d294bdf7b252032528c2fccc88319b42013b748811b1ad8f433a11a7fd`。 |
| ESP32 | UART：`ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、Base `READY container=running`、`STOP=1`、`UNINSTALL=0`、同 boot `SAME_BOOT=2`、`DONE ... released=1`；`scheduled-uart.log` SHA-256 `6b1425881ca460e9fd5c5880129cfd41890a120c5882fb95d1c0fa2b55339301`。 | Base `READY container=empty`；`reboot-uart.log` SHA-256 `7be956893ae032b28127d37c8fe7bbf4424b5559885e6eae8a908cf7d2258787`。 |

官方 NVS parser 对两目标各阶段的活动页 CRC 检查通过，活动 ECS2 blob 从 sequence **6 → 7 → 7**；C3 最终 blob SHA-256 `a47f431e0b160f607ef58032b61002e20fb56866e9c60d42325de3e5d6fd2d72`，ESP32 为 `226f9321efe1738915f87d2012972aa00a6b7994a97fc8c4bb0b4dc512fc552d`。首次运行只改变系统 `nvs@0x9000` **101 B**、otadata **12 B**、`base_store` **353 B**；两个 app 槽、整个包区及 C3 FRP scratch／ESP32 旧 AT 原始区逐字节不变。冷启动后完整 4 MiB Flash 与卸载后的同片 Flash 逐字节一致：C3 SHA-256 `e71985937af853a9ab5854828fdeeb7f8d5fe31cc30c4c3fe24aa3de0aebde4c`，ESP32 `7a0a75aa85222b50dd0daf685c32d44c23de61754e7a38cc985412c8c9e36632`。解析脚本与逐区结果见 `$RUN/analyze_flash.py` 和 `$RUN/<目标>/region-diff.json`。

这份证据证明当前精确锁的**测试签名变体**在两套合成 Flash 中完成 guest 停止、资源回收、产品卸载、独立持久读回及冷启动空绑定。测试 counter guest 没有故意耗尽入口墙钟期限，因此不将本轮 QEMU 运行计为墙钟陷阱验收。正式镜像仍无设备产品操作消费者、安装 API 与结果收据；QEMU 不证明实板 Flash 时延、调度期限、旧持久数据迁移、断电恢复、网络并发或五能力同板验收。C3 候选分区／策略未写回正式源码；ESP32 正式 CSV 只用于仓外测试签名构建，没有设备刷写。
