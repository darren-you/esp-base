# Base 产品卸载的双目标签名 QEMU 检查点

2026-09-27，以 `esp-base@e536b3da6c148d21e90a8fec9106cdee65b736a4` 的独立 Git 归档、固定 ESP-IDF/lwIP、真实签名 ABI 2 counter 包和仓外测试键，在 `mac-work-1` 的隔离合成 4 MiB Flash 上验证产品限定的停止、卸载及重启。**ESP32 与 C3 均在真实 FreeRTOS 测试任务内调用正式 Base API，得到 `stop_confirmed=true`、`product_uninstall=COMPLETE(0)`、同次启动 `product_boot=EMPTY(2)` 和 storage claim 释放成功；同片 Flash 冷启动继续为 `EMPTY`。**这不是正式产品命令或实板验收。

## 输入和测试变体

工作目录为 `mac-work-1:/private/tmp/esp-base-e536-uninstall-qemu-verify-20260927/`，下文以 `$RUN` 表示该绝对路径。`git archive e536b3d` 的 tar SHA-256 为 `e29d2863fdb7e9abba469ac98072a3f1b427b0f1372f1cb028f8bff6891674a2`。固定 SDK 为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；精确 Container/WAMR 锁分别为 `3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd`、`26c235e53e29acd8b43abe7f3b524577bd4d1ae5`。两目标使用 Espressif QEMU `9.2.2 (esp_develop_9.2.2_20260417)`。

正式 `e536b3d` 产品源码没有设备产品操作消费者，原样签名 ELF 的 `nm` 只有 `esp_base_container_product_boot`，没有 `esp_base_container_product_uninstall` 或 `stop_confirmed`，因此不能从原镜像调用卸载。隔离源码副本通过[宿主准备脚本](https://github.com/esp-space/esp-base/blob/0cd9ae1713fae90a1f315fabb9622326d9cfe5f4/tools/prepare_qemu_product_uninstall_probe.py)在 `app_main` 的 READY 之后创建 16 KiB 的 FreeRTOS 测试任务；该任务以唯一 Base storage claim 调用正式 `stop_confirmed → product_uninstall → product_boot → release`，没有替换 Container、WAMR、NVS 或 OTA 实现。传入的是离线 seed 得出的**精确** ECS2 sequence 6 和本轮包 SHA-256。测试源码与生产源码的唯一应用代码差异分别保存在 `$RUN/c3/qemu-probe.diff`（SHA-256 `790fc236d9e0a7984159c37514c7ea9e0415b3a57149b88bb92838b951bd9c52`）与 `$RUN/esp32/qemu-probe.diff`（`c192a0734427243052c4317e561c45c345f6ddf88ba08b07c9af9820fa25b3b5`）；准备脚本对独立 `e536b3d` 归档重放后，生成主源码 SHA-256 精确复现为 C3 `b5ef62f3770682341b2ee0486f55141a77b422f4651a78228952873630708fc1`、ESP32 `d426b9b6a14c451763fddd04af0d6964359e8323974deef67962b839bfb31049`。正式 checkout、正式分区 CSV、签名键与实体设备均未修改。

| 目标 | 仓外几何和签名输入 | 签名 app、官方容量门 | 签名包 |
| --- | --- | --- | --- |
| C3 | `sdkconfig` SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`；候选 CSV `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`；只在仓外把 C3 OTA policy 的 `ota_1` 改为 `0x140000`、槽大小改为 `0x120000`，候选头 SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`；RSA v2 测试键 | `0x111000` B、SHA-256 `75a1a769681efb9aceea9c77aefa7ff6f6dc3446057e8e0a8407fe70669355c1`；双 `0x120000` app 槽各余 `0xf000`；`espsecure verify-signature --version 2 --keyfile <仓外测试键>` 成功 | 10,240 B，SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c` |
| ESP32 | `sdkconfig` SHA-256 `6ddf140b2845f4e1d4d9bc69b4aad06f446715d7b3d41bfe164b1846a3e0d456`；候选 CSV `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`；ECDSA v1 测试键 | `0x10fff4` B、SHA-256 `6011ec0ece91c1cd4658a5d5b31909eab404424450f6105ebd9891e60d569c8a`；双 `0x120000` app 槽各余 `0x1000c`；app 与签名分区表的 `espsecure verify-signature --version 1 --keyfile <仓外测试键>` 均成功 | 10,240 B，SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e` |

仓外 seed 使用锁定 Container 的正式 `slots.c`、包 validator、包签名与 Wasm 校验，内存 Flash/NVS IO 只负责**制造前置状态**。它按 `initialize → reserve → write_and_prepare → begin_trial → mark_healthy → confirm → reconcile` 将签名包绑定到本轮刚编译的完整签名 app，生成 ECS2 sequence 6、`CONFIRMED`，再由固定 SDK 的 NVS V2 generator 写入 `base_pkg/slots`。C3 的预置 ECS2 blob SHA-256 为 `a47851e0348db2c312c4ea4dd4bba2f77ee1225ab336cb374a486560064dd686`；ESP32 为 `3fc59abe0c80e1adaf8ee6b7617edfe5372b7dddebc6e0935e11ed5aaabe289e`。前置安装**不是**设备 Base API 行为。C3 合成 Flash SHA-256 为 `52f5de81b43780d0fced82032341565b063edd378c846b277548c9c1a19e0442`；ESP32 为 `7bf2395cc3b76e21120391747ba3cf897b5319b67b1d926b05d434420317b042`。各输入偏移和逐段摘要在 `$RUN/<目标>/flash-receipt.json` 与原始构建目录；两目标包分区均为 `0x260000/0x186000`，三包槽各 `0x82000`。

## 精确执行与观察

准备脚本只对**独立归档目录**复现本轮测试源码；两个包 SHA 对应各自真实 seed 输出。实际构建命令及 QEMU 启动命令保留在 `$RUN/<目标>/build*.log`、`$RUN/c3/{uninstall,reboot}-command.json` 与 `$RUN/esp32/{scheduled,reboot}-command.json`。对应的可复现命令为：

```bash
python3 tools/prepare_qemu_product_uninstall_probe.py \
  --source-root "$RUN/c3/source" \
  --package-sha256 43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c \
  --expected-sequence 6
python3 tools/prepare_qemu_product_uninstall_probe.py \
  --source-root "$RUN/esp32/source" \
  --package-sha256 9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e \
  --expected-sequence 6
idf.py -C "$RUN/c3/source/firmware" -B "$RUN/c3/build" \
  -DSDKCONFIG="$RUN/c3/sdkconfig" -DIDF_TARGET=esp32c3 build
idf.py -C "$RUN/esp32/source/firmware" -B "$RUN/esp32/build" \
  -DSDKCONFIG="$RUN/esp32/sdkconfig" -DIDF_TARGET=esp32 build
python3 "$RUN/c3/esp-base-e536-c3-runner.py" uninstall 23460
python3 "$RUN/c3/esp-base-e536-c3-runner.py" reboot 23461
python3 "$RUN/esp32/run-scheduled.py" scheduled
python3 "$RUN/esp32/run-scheduled.py" reboot
```

构建前还须放入上述仓外候选 CSV、C3 policy 两行和 sdkconfig／测试签名键；`seed` 按前述正式 Container 源码生成 ECS2/包区，固定 SDK 的 `nvs_partition_gen.py generate ... --version 2` 生成 NVS。合成 Flash 只按各目标 `flash_args` 写入 bootloader、候选分区表、otadata、签名 app、包区和 `base_store`，每段写后按源字节回读。`uninstall` 与 `scheduled` 使用初始 Flash 的独立副本，`reboot` 使用首次运行后**同片** Flash；QEMU 参数和 GDB 命令脚本的原文在上述目录。C3 每次启动只在 `adc2_init_code_calibration` 入口通过 GDB 将模拟 PC 设为返回地址，绕过 QEMU 未模拟的 ADC2 校准；没有改变签名 app 字节。ESP32 直接从 UART 观察，不作此跳过。

| 目标 | 首次运行的实际状态 | 同片冷启动 | 原始证据 |
| --- | --- | --- | --- |
| C3 | GDB 命中测试任务完成点：`stage=4`、`stop=1`、`uninstall=0`、`reopen=2`、`release=1`；storage claim `{owner=0,token=0}` | READY 路径上 `s_product.result=2`（`EMPTY`）、`thread_joinable=false`、`instance_active=false`、claim 为 0 | `$RUN/c3/uninstall-gdb.log` SHA-256 `527f65709faf70c58dd4281307776c4cabde9d66c5c4c8fffc8afe472d2b66d1`；`reboot-gdb.log` `ba11727757ea4e3f224450e23c9218720fddc96726af6ffe6c7db2724e762fca` |
| ESP32 | UART：`ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、Base `READY container=running`、`QEMU_PROBE_STOP result=1`、`UNINSTALL result=0`、`SAME_BOOT result=2`、`DONE ... released=1`；无 panic | UART：Base `READY container=empty`；测试任务因没有运行 guest 报 `STOP result=0`，没有重复卸载 | `$RUN/esp32/scheduled-uart.log` SHA-256 `1d9567bd4b6ee1487e7fd0158053af893a516a1556ec587400980ca350be7b43`；`reboot-uart.log` `4f77c086fba6bf90ebe2fa91b02a3b2002710f5ed0f8749f347318a65e473340` |

C3 固件把产品控制台放在 USB Serial/JTAG，而此 QEMU 的 `-serial` 只接 UART0；两次串口文件都只有 236 B ROM 入口，不能拿它们宣称产品日志。GDB 读取的是签名测试镜像中真实任务执行后的变量，不是 GDB inferior call 的卸载返回。此前一次 ESP32 **GDB 从 3584 B 的 `app_main` 栈直接调用卸载**触发 `***ERROR*** A stack overflow in task main`，原始 `$RUN/esp32/uninstall-gdb.log`／`uninstall-uart.log` 已保留；该尝试不计卸载成功，也没有通过改高生产 main 栈来掩盖。成功路径的 main 栈仍为 3584 B，16 KiB 的独立测试任务由 FreeRTOS 正常调度。

## 持久读回与边界

两目标的官方 NVS parser 均确认 `base_store` 活动页 CRC 正确。按官方 NVS V2 结构提取的活动 ECS2 blob，均由初始 sequence **6** 变为卸载后 **7**，冷启动后保持 7：C3 最终 blob SHA-256 `fa422a182516a46da5b1159416fdc7c50b0d8a6d51b8a4f31ba4d37262a24ebe`，ESP32 为 `8a2d5874e426136c1b9833a0b41deecc965d1216241762c0652c7216e9948080`。Base `product_uninstall` 的 `COMPLETE` 本身还要求独立 ECS2 读回、无当前包绑定、回退固件绑定未变及签名固件集合再观察；下一次真实产品启动得到 `EMPTY`。

两目标首次运行相对预置 Flash 都只改变系统 `nvs@0x9000` **101 B**、otadata **12 B**、`base_store` **353 B**；`ota_0`、`ota_1`、整个包区和 FRP scratch 逐字节不变。冷启动后的完整 4 MiB Flash 与卸载后逐字节一致。C3 卸载后／冷启 SHA-256 均为 `eda510e3c89664df7081417eab8c2a71a5ac887cb82f2e5069b4adeac7df2538`；ESP32 均为 `d89bb241b87c78c1cf44b8545260a09d7dc2a49195393195dddc78bb8ca80924`。逐区结果保存在 `$RUN/<目标>/region-diff.json`。

证据层级是**测试签名镜像 + 仓外候选几何 + 合成 Flash 的 QEMU 真运行**。它证明 Base 当前内部 API 在这两个受控测试上下文完成停止、join、卸载提交、独立读回和重启空绑定；不证明原样产品镜像可接收卸载请求。当前没有 `product.*` 设备命令、Base 公开安装 API、设备端结果收据或业务事件源，因此无法从本轮卸载后的设备状态经 Base API **重新安装**；原始包 Flash 保留不等于已重装。本轮也没有实体设备写入、正式分区迁移、旧 AT／C3 v1 数据、掉电原子性、真实 Flash 时延、FRPS/Broker/HTTPS 并发或实板资源峰值证据，P6-03/P7-02 不据此验收。
