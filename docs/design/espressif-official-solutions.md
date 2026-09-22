# 乐鑫官方仓库与 ESP Base 固件选型

核对日期：2026-09-22。本文针对当前 `esp-base` 的 ESP32-C3、4 MiB、ESP-IDF v6.1 基线；[逐仓清单](./espressif-repository-catalog.md)记录当日乐鑫 GitHub 组织的全部公开仓库。组织名下有源码、组件、例程、文档、工具链、硬件资料、上游 Fork 和归档项目。组织归属本身不代表某仓可作为本固件的生产依赖。

## 结论

目前最值得采用的是**已有的 ESP-IDF 基础能力**、实现网络时的 `esp_wifi` / `esp_netif` / `esp_event`、实现 MQTT 时的官方 `espressif/mqtt`、实现 OTA 时的 `esp_https_ota` / `app_update`。组件依赖应由 IDF Component Manager 精确声明并提交锁文件。`esp-matter`、`esp-rainmaker`、`esp-bsp` 和整套无线配网目前没有已确认的需求或板型依据，不能因为它们也是官方仓库就纳入基座。FRP 不是乐鑫官方协议组件，仍按本仓既有公开 `esp-frp` 边界单独验证。

这是一份**选型与实施顺序**，没有修改固件，也不表示以下待实现能力已经通过实板验收。

## 事实边界

| 项目 | 当前事实 | 代码证据 |
| --- | --- | --- |
| 构建目标 | ESP-IDF v6.1、ESP32-C3、4 MiB、USB Serial/JTAG；开启 OTA rollback 与 Task WDT | [仓库 README](../../README.md)、[sdkconfig.defaults](../../firmware/sdkconfig.defaults) |
| Flash | `ota_0`、`ota_1` 各 `0x1e0000`，另有 NVS、otadata、coredump、`base_store`；单槽上限 1,966,080 字节 | [分区表](../../firmware/partitions/partition_table.csv) |
| 已运行的入口 | NVS 初始化、持久 UUID 与芯片事实、复位原因、配置 `generation` 读取、运行 OTA 槽读取、5 秒串口心跳 | [主程序](../../firmware/apps/esp_base/main/esp_base_main.c)、[心跳](../../firmware/components/device_protocol/esp_base_protocol.c) |
| 尚未实现 | Wi-Fi、MQTT、FRP、配置事务、OTA 下载和新启动确认；命令 guard 仅有 host 测试，尚未接入串口/网络执行 | [开发检查点](../operations/development-checkpoint.md)、[协议设计](./device-protocol.md) |
| 设备验收 | 当前构建和 ASan/UBSan host 测试通过；尚未烧录本轮实板 | [开发检查点](../operations/development-checkpoint.md)、[测试说明](../../firmware/tests/README.md) |

特别注意：心跳当前用 32 位随机数打印 8 位十六进制 `boot_id`，而命令 guard 要求 UUID v4；设计协议称配置版本为 `revision`，迁入代码只读 `generation`。在 USB 与网络共用命令入口前，须先确定并实现同一身份、配置和结果合同，不能用 transport 库掩盖这个差异。证据见[心跳](../../firmware/components/device_protocol/esp_base_protocol.c)、[命令 guard](../../firmware/components/device_protocol/command_guard.c)、[配置读取](../../firmware/components/remote_config/esp_base_remote_config.c)及[协议设计](./device-protocol.md)。

## 官方方案映射

| 能力与来源 | 对 ESP Base 的判断 | 接入边界和完成证据 |
| --- | --- | --- |
| [ESP-IDF v6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1) | **已采用，继续作为唯一设备 SDK**。NVS、分区/OTA、事件循环、Wi-Fi、TLS、HTTP 和 WDT 可直接使用 IDF 组件。 | 保持 `esp32c3` 与分区表；构建、锁定依赖和实板测试必须基于同一 SDK 版本。不要另建 Arduino 或 MicroPython 运行面。 |
| [ESP-IDF Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/network/esp_wifi.html)、[esp_netif](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/network/esp_netif.html)、`esp_event` | **实现 Wi-Fi 时优先采用**，由 SDK 负责 STA 连接与 IP 事件；`remote_config` 保留已提交/候选配置的业务裁决。 | 候选凭据使用 `WIFI_STORAGE_RAM`，避免 Wi-Fi 默认 Flash 持久化绕过配置事务；连接、取得 IP 与必要链路 proof 后才提交本仓配置。断网、超时、重启和旧配置恢复须实板验证。 |
| [ESP-MQTT 仓库](https://github.com/espressif/esp-mqtt) / [组件 `espressif/mqtt`](https://components.espressif.com/components/espressif/mqtt) | **实现 MQTT 时采用**。IDF v6.1 已将 MQTT 移至组件管理器；这是本仓规则指定的官方客户端。 | 增加精确版本组件依赖和 `dependencies.lock`；在真实 HK Broker 合同下验证 TLS、重连、订阅和发布。MQTT 只提供传输，`device_id`、HMAC、`boot_id`、期限、队列和最终结果仍由[自有协议](./device-protocol.md)裁决。 |
| [ESP HTTPS OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/esp_https_ota.html) / [IDF OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/ota.html) | **实现 OTA 时采用**，用 SDK 完成 HTTPS 下载、写入非运行槽和 bootloader rollback。当前代码只检查运行槽，不能算完成 OTA。 | 本仓仍验证 operation_id、目标芯片/分区、长度、SHA-256、签名元数据和新启动自检；只有自检成功才调用 `esp_ota_mark_app_valid_cancel_rollback()`。检查断电、错误镜像、回滚及 1,966,080 字节单槽限制。 |
| [IDF Component Manager](https://github.com/espressif/idf-component-manager) | **随第一个外部官方组件一起采用**，不手工复制 MQTT 源码。 | `idf_component.yml` 声明精确依赖，提交求解生成的 [`dependencies.lock`](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/dependencies_lock.html)；在全新 checkout 上复现构建。当前仓尚无这两个跟踪文件。 |
| [NVS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_flash.html)、[Task WDT](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/wdts.html)、[Core Dump](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/core_dump.html) | **沿用已启用的基础设施，按实际功能补全**。身份、配置、复位事实和 coredump 分区已有基线。 | 不自动擦 NVS；确认配置事务的提交与掉电恢复；诊断从真实故障读取。分区存在不等于 coredump 已完成采集链路。 |
| [pytest-embedded](https://github.com/espressif/pytest-embedded)、[esptool](https://github.com/espressif/esptool) | **在实板验收阶段使用**官方测试与设备识别/烧录工具；当前 host 测试继续保留。 | 测试脚本每次确认真实芯片、分区和两份完整 Flash 恢复基线；构建、host 测试与 target 测试分别报告。刷写必须另获当前设备与恢复基线授权。 |

其中 [`ESP-IDF v6.1 Release`](https://github.com/espressif/esp-idf/releases/tag/v6.1)明确写明 MQTT 已移至 Component Manager；[`ESP-MQTT` 组件说明](https://components.espressif.com/components/espressif/mqtt)给出 IDF 依赖方式。SDK 文档说明 `esp_https_ota` 负责传输与写入，OTA 成功后的业务确认仍须由应用决定。

### 条件候选

| 官方仓库/方案 | 仅在什么事实成立时引入 | 当前判断 |
| --- | --- | --- |
| [IDF 统一配网](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/provisioning/provisioning.html)、[移动端配网客户端](https://github.com/espressif/esp-idf-provisioning-android) | 产品明确要求手机 BLE/SoftAP 首配，并决定与现有 USB JSON Lines 首配的授权和配置事务关系。 | 当前不引入第二首配入口。可借鉴其会话安全与配网测试，但不能直接替代现有协议。 |
| [esp-protocols](https://github.com/espressif/esp-protocols) | 出现明确的 mDNS、WebSocket、蜂窝等网络协议需求时，按单组件评估。 | 当前 Wi-Fi、HTTPS OTA 与 MQTT 的 SDK/专用组件已覆盖已知需求；此仓没有 FRP 替代组件。 |
| [esp-iot-solution](https://github.com/espressif/esp-iot-solution)、[esp-bsp](https://github.com/espressif/esp-bsp) | 确定具体外设或板型、引脚和真实驱动需求后挑对应驱动/BSP。 | 当前无 GPIO 输出、无已确认外设板型；“同为 ESP32-C3”不足以证明 BSP 匹配。 |
| [esp-matter](https://github.com/espressif/esp-matter) | 产品真的需要 Matter 互通、commissioning、设备模型和认证。 | 当前协议为自有 USB/MQTT/FRP。其当前 README 推荐 IDF v6.0.2，若目标改变须重新验证 v6.1 与 4 MiB 双槽。 |
| [esp-rainmaker](https://github.com/espressif/esp-rainmaker)、[esp-insights](https://github.com/espressif/esp-insights) | 决定使用 RainMaker 云端控制/诊断、设备申领和相应 App 生态。 | 当前已有自有命令与 Broker/FRP 边界，不引入另一控制面或云依赖。 |

`esp-idf` 示例的 [`example_connect()`](https://github.com/espressif/esp-idf/blob/master/examples/protocols/README.md)可参考初始化顺序，官方说明它没有超时和完整错误恢复；产品连接状态应由本仓运行逻辑裁决，不能把示例直接当生产连接管理。

## 建议实施顺序与验收点

1. 先收敛 `boot_id`、`revision`、USB/网络命令的字段和错误语义，接通现有 guard 与执行/结果队列；用协议测试证明重复投递、过期、重启和目标绑定。
2. 用 IDF Wi-Fi/网络事件实现 RAM 候选连接与配置事务；实板验证候选失败和掉电后保留旧配置。
3. 用精确锁定的 `espressif/mqtt` 实现 TLS 网络命令通路；在 Broker 中断与重连时验证最终结果不会被 PUBACK 冒充。
4. 用 `esp_https_ota` 和 bootloader rollback 完成 OTA；在 ESP32-C3 双槽上量测镜像大小、运行堆、下载峰值、断电恢复与新启动自检。只有实板通过才更新 README 的已支持状态。
5. FRP 继续按本仓公开 `esp-frp` 来源、许可和资源边界独立评估；它不属于乐鑫 342 仓所提供的官方替代方案。

这份顺序只选择与当前[设备协议](./device-protocol.md)和[嵌入式工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded-firmware/embedded-firmware-golden-path.md)一致的技术能力。任何 Flash 写入、整片擦除、eFuse、安全启动或加密配置变更，均应先满足该标准的精确设备和恢复基线要求。
