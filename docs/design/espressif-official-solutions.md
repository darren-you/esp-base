# 乐鑫官方仓库与 ESP Base 固件选型

仓库清单核对日期：2026-09-22；实现状态核对日期：2026-09-24。本文针对当前 `esp-base` 的 ESP32-C3、4 MiB、ESP-IDF v6.1 基线；[逐仓清单](./espressif-repository-catalog.md)记录当日乐鑫 GitHub 组织的全部公开仓库。组织名下有源码、组件、例程、文档、工具链、硬件资料、上游 Fork 和归档项目。组织归属本身不代表某仓可作为本固件的生产依赖。

## 结论

当前已采用 ESP-IDF 基础能力、`esp_wifi` / `esp_netif` / `esp_event`、公开 `esp-mqtt` 内的官方 ESP-MQTT 核心及 OTA `app_update` 的 pending 本地确认。受控签名构建的软件 OTA 下载采用官方 `esp_http_client` HTTPS 与 `app_update`；组件依赖由 IDF Component Manager 精确声明并提交锁文件。`esp-matter`、`esp-rainmaker`、`esp-bsp` 和整套无线配网目前没有已确认的需求或板型依据，不能因为它们也是官方仓库就纳入基座。FRP 不是乐鑫官方协议组件，仍按本仓既有公开 `esp-frp` 边界单独验证。

这是一份**选型与实施顺序**；软件编译通过不表示签名 OTA 已通过实板验收。

## 事实边界

| 项目 | 当前事实 | 代码证据 |
| --- | --- | --- |
| 构建目标 | ESP-IDF v6.1、ESP32-C3、4 MiB、USB Serial/JTAG；开启 OTA rollback 与 Task WDT | [仓库 README](../../README.md)、[sdkconfig.defaults](../../firmware/sdkconfig.defaults) |
| Flash | `ota_0`、`ota_1` 各 `0x1e0000`，另有 NVS、otadata、coredump、`base_store`；单槽上限 1,966,080 字节 | [分区表](../../firmware/partitions/partition_table.csv) |
| 已装配的入口 | NVS 持久身份、复位事实、已提交配置读取/条件提交、Wi-Fi station、USB status/restart/config.set、5 秒心跳；普通基座 MQTT 网络命令和 FRP loopback 只读 status 均有软件接线；签名构建还装配 ota.start/HTTPS OTA，pending 新槽有本地确认与失败回滚接线 | [主程序](../../firmware/apps/esp_base/main/esp_base_main.c)、[控制任务](../../firmware/components/device_protocol/esp_base_protocol.c)、[OTA 产品约束与收据](../../firmware/components/ota_operation/README.md) |
| 尚未验收 | 签名基座的首次实板迁移与 OTA 实板验收、普通基座 MQTT/FRP 真实远端请求到板及同板资源并行；FRP 当前仅接受只读 status | [开发检查点](../operations/development-checkpoint.md)、[协议设计](./device-protocol.md) |
| 设备验收 | 本轮 OTA 仅有 host 故障注入、普通与隔离签名 C3 编译，没有 HTTPS 下载、新槽及回滚实板验收；既有 Wi-Fi/配置/MQTT 实板记录另见检查点 | [开发检查点](../operations/development-checkpoint.md)、[测试说明](../../firmware/tests/README.md) |

USB 命令已使用 UUID v4 `boot_id`、配置 `revision` 与有界请求裁决；MQTT 与 FRP 只读 status 网络入口的软件接线仍需真实设备闭环。OTA pending 自检和签名下载期间配置写入与 OTA 互斥，控制循环 5 秒活性阈值和真实回滚仍需实板负载证明。证据见[控制任务](../../firmware/components/device_protocol/esp_base_protocol.c)、[配置存储](../../firmware/components/remote_config/esp_base_remote_config.c)及[协议设计](./device-protocol.md)。

## 官方方案映射

| 能力与来源 | 对 ESP Base 的判断 | 接入边界和完成证据 |
| --- | --- | --- |
| [ESP-IDF v6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1) | **已采用，继续作为唯一设备 SDK**。当前源码固定公开 [ESP-IDF 维护 fork](https://github.com/esp-space/esp-idf/tree/codex/fix-http-init-transport-oom) `578cf89c343e388db43ba1f4ddcd602fedcb763c` 及 esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d`；NVS、分区/OTA、事件循环、Wi-Fi、TLS、HTTP 和 WDT 仍使用 IDF 组件。 | 保持 `esp32c3` 与分区表；仓根 `sdk-lock.json` 和构建守卫核对 SDK，实板测试须使用同一源码提交。不要另建 Arduino 或 MicroPython 运行面。 |
| [ESP-IDF Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/network/esp_wifi.html)、[esp_netif](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/network/esp_netif.html)、`esp_event` | **已在普通基座采用**，由 SDK 负责 STA 连接与 IP 事件；`remote_config` 裁决已提交/候选配置。 | 候选凭据使用 `WIFI_STORAGE_RAM`，避免 Wi-Fi 默认 Flash 持久化绕过配置事务；连接、取得 IP 与必要链路 proof 后才提交本仓配置。外部 AP/WPA3 等矩阵继续见开发检查点。 |
| [ESP-MQTT 仓库](https://github.com/espressif/esp-mqtt) / [公开维护仓](https://github.com/esp-space/esp-mqtt) | **隔离测试应用和普通基座均已采用公开组件**，固定 `esp-mqtt@5bff093646d8db810d64c50c39edc004e78bf40c`；普通基座在具备已提交凭据和可信时间后启动 MQTT owner。 | 旧适配层曾验证隔离 TLS Broker；新仓已有真实核心的 Linux 隔离 Broker 断线及 QoS1 ACK/重投回归和固定 SDK C3 编译。普通基座已接入 v3 持久配置、HMAC 命令与结果通道；真实设备 Broker/TLS、命令 ACK、账户 ACL 和配置迁移仍待验收。`device_id`、`boot_id`、期限、队列与最终结果由[自有协议](./device-protocol.md)裁决。 |
| [ESP HTTP Client](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/protocols/esp_http_client.html) / [IDF OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/ota.html) | **签名构建的软件链已接入**：USB 命令、HTTPS 下载、完整 signed bin 长度/摘要、SDK 验签/切槽、pending 本地确认；普通未签名构建拒绝 OTA。 | 临时测试键签名构建和 host 故障注入已过；首次签名基座迁移、旧 bootloader 能力、真实 TLS/坏签名/断流/回滚/断电仍需实板验收。单槽 1,966,080 字节上限含签名 padding 与签名扇区。 |
| [IDF Component Manager](https://github.com/espressif/idf-component-manager) | **已采用**，不手工复制 MQTT、OTA 或 cJSON 源码。 | `idf_component.yml` 声明公开 MQTT、OTA Git 完整提交与 cJSON 版本，[`dependencies.lock`](../../firmware/dependencies.lock) 固定求解结果；在全新 checkout 上复现构建。 |
| [NVS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_flash.html)、[Task WDT](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/wdts.html)、[Core Dump](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/core_dump.html) | **沿用已启用的基础设施，按实际功能补全**。身份、配置、复位事实和 coredump 分区已有基线。 | 不自动擦 NVS；确认配置事务的提交与掉电恢复；诊断从真实故障读取。分区存在不等于 coredump 已完成采集链路。 |
| [pytest-embedded](https://github.com/espressif/pytest-embedded)、[esptool](https://github.com/espressif/esptool) | **在实板验收阶段使用**官方测试与设备识别/烧录工具；当前 host 测试继续保留。 | 测试脚本每次确认真实芯片、分区和两份完整 Flash 恢复基线；构建、host 测试与 target 测试分别报告。刷写必须另获当前设备与恢复基线授权。 |

其中 [`ESP-IDF v6.1 Release`](https://github.com/espressif/esp-idf/releases/tag/v6.1)明确写明 MQTT 已移至 Component Manager；[`ESP-MQTT` 组件说明](https://components.espressif.com/components/espressif/mqtt)给出 IDF 依赖方式。`esp_http_client` 提供 HTTPS 传输，`app_update` 提供 staged 写入、验签与切槽；OTA 成功后的业务确认仍由应用决定。本机 IDF 6.1 的 `esp_https_ota` 首块读取会在 EAGAIN 时内部重试，因此本基座使用这两个官方底层 API，使普通断流能在每次 SDK 返回后由应用裁决期限。

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
3. 用精确锁定的公开 `esp-mqtt` 实现 TLS 网络命令通路；在 Broker 中断与重连时验证最终结果不会被 PUBACK 冒充。
4. 用 `esp_http_client`、`app_update` 和 bootloader rollback 完成 OTA；在 ESP32-C3 双槽上量测镜像大小、运行堆、下载峰值、断电恢复与新启动自检。只有实板通过才更新 README 的已支持状态。
5. FRP 继续按本仓公开 `esp-frp` 来源、许可和资源边界独立评估；它不属于乐鑫 342 仓所提供的官方替代方案。

这份顺序只选择与当前[设备协议](./device-protocol.md)和[嵌入式工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded_firmware/embedded_firmware_golden_path.md)一致的技术能力。任何 Flash 写入、整片擦除、eFuse、安全启动或加密配置变更，均应先满足该标准的精确设备和恢复基线要求。
