# 产品卸载串口协议签名 QEMU 检查点

2026-09-28，基于 `esp-base@16984b23bfb28f1b37cec38665b191a737d7656b` 和本次控制任务栈修正，在 `mac-work-1:/private/tmp/esp-base-16984b2-protocol-qemu-20260928/esp32/` 的独立源码、测试键和合成 4 MiB Flash 中，向**正式 Base 串口协议**发送 `product.status → product.uninstall → product.result`，再对同片 Flash 冷启动，查询原操作并重发同一 `operation_id`。这次没有加入测试任务或直接调用内部卸载函数；前置签名产品绑定由仓外 seed 制造，设备产品写入口、EPRD 账本与 Container 卸载均由正式 app 执行。

## 栈故障与修正

原 ESP32 `base_control` 为 4,096 B。签名 guest 启动后，`product.status` 调用 SDK 签名固件集合验证时触发 `***ERROR*** A stack overflow in task base_control`。仅提高到 6,144 B 后，状态查询成功，但 `product.uninstall` 的完整停止、验签和持久提交路径仍触发同一溢出。8,192 B 完成下述全链；本次源码只提高 ESP32 控制任务栈，C3 保持原有 6,144 B。失败输入分别保存在仓外 `failure-4k/`、`failure-6k/`，成功输入保存在 `esp32/`，均使用同一测试签名包和对应 app 摘要重建的 ECS2 前置绑定。

## 精确输入与执行

- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，Espressif QEMU `9.2.2 (esp_develop_9.2.2_20260417)`；本次未改正式组件锁和分区表。
- 最终源码的 ECDSA v1 测试键签名 app 为 `0x10fff4` B，SHA-256 `3df1dc34bd730f89c606a54d74b5683784047ef96b1ee889893eae24f72bdb29`；app 和签名分区表均通过官方 `espsecure verify-signature --version 1`，app 槽剩余 `0x1000c` B。测试键不入仓，也不是设备生产键。
- 签名 ABI 2 counter 包 SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`。仓外 `seed` 使用当前锁的 Container 槽与签名包验证源码，针对**最终 app 完整摘要**生成 ECS2 sequence 6、已确认绑定与真实包分区；EPRD v1 初始空账本为 910 B。官方 NVS generator 生成 `base_pkg/slots` 与 `base_product/operations`，bootloader、签名表、otadata、app、包和 Base NVS 按正式偏移写入 `seeded-flash-final.bin`，每区写后读回。
- 初始 Flash SHA-256 `5be2618414c59d97d6603a92587782d4d31e96c8dfab255a6144c58ddeb19a2d`。仓外 `flash-receipt-final.json`、`seed-final.log`、`nvs-gen-final.log`、`initial-command.json`、`reboot-command.json` 和两个 UART 日志保留输入与 QEMU 命令。`run_protocol.py` 在 UART TCP 发送正式 JSON，先执行首次运行，再以其写出的 `initial-flash.bin` 冷启动；QEMU 的 `-no-reboot` 保证溢出不会被自动重启掩盖。

## 设备协议和持久结果

首次 `ESP_BASE_READY container=running`，`product.status` 报 ECS2 sequence 6、包摘要正确、下一操作序号 1。带当前 `device_id`、`boot_id`、期限、原包摘要和 sequence 6 的 `product.uninstall` 返回 `succeeded`；原操作 ID `a1e14248-e5f2-4332-9cd6-6436ce28ca75` 的 `product.result` 返回 `kind=uninstall`、`operation_sequence=1`、`container_sequence=7`、`result_code=0`。

同片冷启动得到新的 `boot_id`、`ESP_BASE_READY container=empty`；`product.status` 返回 ECS2 sequence 7、空包绑定、账本高水位 1 与下一序号 2。按原 ID 查询仍成功；使用**新的 request ID 与本次 boot 身份**重发同一 `operation_id` 返回成功，随后状态仍为 sequence 7／高水位 1，没有再次卸载。`initial-uart.log` 和 `reboot-uart.log` 保存逐条原始回执。

官方 NVS parser 对最终 Flash 中 `base_store@0x3fa000` 的活动页均报 CRC OK；独立解码的 ECS2 288 B、EPRD 910 B 内部 CRC 也均正确。首次卸载与冷启动后的 ECS2 blob SHA-256 同为 `39d15d1f73d1e85dd9e1d8093d183085f340cb159eae4e6a2ef7d2d6152afa83`，账本 blob 同为 `ee989dccb7e9dae0de24bafb825c570df9350e58b58fe6be73a5ec0f59a16534`；账本只有一条 `uninstall/succeeded`，序号／高水位 1、Container sequence 7。首次运行后的完整 Flash 与冷启动后逐字节一致，SHA-256 `cf58cec5b6306337c615f9818e4c62a3ae6a9d12698156862b53dc08fb363593`。相对初始镜像仅系统 NVS、otadata、Base NVS 分别改变 101、12、2,315 B；两个 app 槽和整个包区逐字节不变。

本次源码在固定 SDK 的 C3／ESP32 普通完整构建及两目标 `run_host_tests.sh` ASan／UBSan 回归均通过；最终签名 ESP32 app 也在重新生成签名绑定后重复通过 QEMU 串口全链。QEMU 本次无 Wi-Fi 配置、Broker、FRPS 或 HTTPS 负载，报告的 121,852 B 堆历史低水**不是**五能力同存峰值。未执行实体设备写入、真实掉电、公开安装／升级和带业务事件的试运行确认；P6-03、P6-04、P7 仍按各自完整门槛继续验收。
