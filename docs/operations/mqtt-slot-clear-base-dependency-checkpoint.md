# MQTT 消息槽清零的 Base 精确消费检查点

2026-09-27，从 Base `0babeec8f39f8af528e23ffa97e94bc77ecd2f79` 出发，先合入独立资源归因文档 `7fefaad`，再把主固件唯一 Component Manager 清单中的 MQTT 精确版本提升到公开 `esp-mqtt@c0677e5e779c3e51e814f2920420be7ec54f1d88`。MQTT 提交在运行层复用固定入站消息槽时清零旧 Topic／payload 尾部；新增宿主回归在旧实现断言失败，新实现通过。本次 Base 不改 MQTT owner、设备命令、分区、运行策略或其他组件版本。

## 固定输入与双目标构建

- 构建在 `mac-work-1:/private/tmp/esp-base-mqtt-slot-clear-20260927/` 的独立源码副本完成。源码副本从本分支复制时只包含固件清单变更，后续归因与检查点文档不影响固件源码。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 通过本仓 `tools/check_sdk.py`。从独立副本移除旧两目标生成锁后，官方 Component Manager 分别为 `esp32c3` 和 `esp32` 重新求解；没有手填组件摘要。
- C3 `firmware/dependencies.lock` SHA-256 为 `ea7af6ed2a720b75d95eca3ce6271a1ab68be8e7afddc0cd7e4cde8debd46d15`，ESP32 `firmware/dependencies.lock.esp32` 为 `7e29444417cba55771bf0a1eaa1625264b1cced24405edccee9da3112b32b616`。两份锁的 `mqtt.version` 均为上述完整 SHA，`mqtt.component_hash` 均为 `7837059bb46e257033dde0d02cc9ae014267a5e09e9a1dbff7f36641d611dd36`，`manifest_hash` 均为 `4f97347ed739c153f73055501f5c744a131088af66d93d0f2bef3a7da479061c`；相对父提交，锁内只有 MQTT 版本、组件摘要和 manifest 摘要变化。受管 `runtime/emqtt.c` SHA-256 为 `b51473ac435e1e9b6e700c2c7f65b0812e4b5f029f56daa9b2bdaab159077590`，与 MQTT 源提交一致，两目标 `compile_commands.json` 均包含该源文件。
- 双目标主固件完整构建通过。C3 普通未签名 app `913344 B`、SHA-256 `09bfdae1d8a0809d406cf0348299c83bb6d06bd817f556103ecf83f4c45cbd95`；ESP32 显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON` 未签名离线 app `863168 B`、SHA-256 `7a038d98ebbe3abc2209ada5ee642f14d3c4709d28ff4aec73b5addc67c5d955`。构建日志分别为该隔离目录的 `build-c3.log`（SHA-256 `98ffe5e0be0c394c84b565f1a739fc83f67f2765802ee12bf0abf86019c76be3`）与 `build-esp32.log`（`2b08527ce4f11eb711f2a728463398e43027af6cce05d64fe77f2425e05e1d56`）。ESP32 离线 app 不是可刷写候选；本轮没有签名构建。

## 宿主回归与边界

Base `firmware/tests/run_host_tests.sh` 使用本次解析的受管组件，在 C3 **20/20**、ESP32 **19/19** 项均通过 ASan/UBSan，原始日志 SHA-256 分别为 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`、`e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`。NVS 容量探针只消费 Container/WAMR，不含 MQTT，其清单和两份锁保持原样。MQTT 源仓自身的消息槽回归与工具 8/8 已通过；此处的 Base host 测试没有接 Broker。

本次只证明公开修正已被 Base 两目标**精确解析并实际编译**，没有连接 Wi-Fi／Broker、验证 TLS/ACL、执行设备命令最终 ACK、签名产品包、实板资源或 Flash。此前 [ESP32 资源归因](esp32-product-resource-attribution.md)在旧 MQTT 锁与仓外签名 guest QEMU 中，无认证探针对照的启动 minimum 为 **43,636 B**，低于 48 KiB 门的 **49,152 B**；新 MQTT 锁没有重跑该组合，不能继承或消除该资源失败。P3-07、P3-08、P6-03 和 P7-02 均未因此验收。
