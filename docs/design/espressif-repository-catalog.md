# 乐鑫 GitHub 公开仓库逐项清单

快照日期：2026-09-22。此清单对应[乐鑫 GitHub 组织仓库页](https://github.com/orgs/espressif/repositories)当日显示的 **342 个公开仓库**；从 [GitHub 官方 REST API 第 1 页](https://api.github.com/orgs/espressif/repos?type=public&per_page=100&page=1)、[第 2 页](https://api.github.com/orgs/espressif/repos?type=public&per_page=100&page=2)、[第 3 页](https://api.github.com/orgs/espressif/repos?type=public&per_page=100&page=3)、[第 4 页](https://api.github.com/orgs/espressif/repos?type=public&per_page=100&page=4)取得元数据，以仓库名去重后逐项归类。每个仓库只归入一个主用途类别；多用途仓的完整能力仍须回到仓库核查。与 `esp-base` 的取舍见[官方方案选型](./espressif-official-solutions.md)。

中文用途优先根据仓库官方简介归纳；简介缺失时读取根 README。75 个仓库没有官方简介，其中 69 个有可读取的根 README；其余 6 个按名称谨慎推断。全表有 11 项因来源过少而明确标为“推断/待核”。`Fork` 标记表示该组织账号下的上游分叉，`归档` 表示 GitHub 已归档；两种状态可以同时存在。没有状态标记只表示这两项均为否，**不表示活跃维护或适合生产采用**。组织仓库清单包含工具、文档、硬件、示例和第三方分叉，不能把全部仓库当作 ESP-IDF 组件。

## 分类概览

| 主用途类别 | 仓库数 |
| --- | ---: |
| 核心 SDK 与应用框架 | 27 |
| 无线通信与网络协议 | 30 |
| 安全、启动与证书 | 15 |
| 芯片底层、板级与硬件 | 34 |
| 烧录、调试与测试 | 35 |
| 开发环境、构建与 CI | 57 |
| 工具链、系统移植与第三方依赖 | 33 |
| 云平台、生态与移动端 | 37 |
| 音视频、图形与人工智能 | 42 |
| 文档、教程与演示 | 32 |
| **合计** | **342** |

其中 **44 个 Fork**、**29 个归档**，有 4 个同时属于两者。分类用于查找，不代表优先级。

## 逐仓用途

表中“依据”表示用途判断的主要公开来源。每个仓库名链接到原仓，便于核对 README、许可、版本和具体芯片支持。

### 核心 SDK 与应用框架（27）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | ESP32 系列芯片的 Arduino Core、板卡定义和配套示例。 | — | 官方简介 |
| [esp-adf](https://github.com/espressif/esp-adf) | 面向音频与多媒体应用的 ESP Advanced Development Framework。 | — | 官方简介 |
| [esp-adf-libs](https://github.com/espressif/esp-adf-libs) | ESP-ADF 使用的库文件仓库；无简介和根 README，按名称推断。 | — | 推断/待核 |
| [esp-amp](https://github.com/espressif/esp-amp) | ESP 多核或多处理器异构应用开发框架；具体能力需核对仓库文档。 | — | 官方简介 |
| [esp-brookesia](https://github.com/espressif/esp-brookesia) | 面向 AIoT 设备的人机交互开发框架。 | — | 官方简介 |
| [esp-bsp](https://github.com/espressif/esp-bsp) | 乐鑫开发板的板级支持组件集合。 | — | 官方简介 |
| [esp-gmf](https://github.com/espressif/esp-gmf) | 乐鑫通用多媒体框架，组织音视频处理流水线。 | — | 官方简介 |
| [esp-idf](https://github.com/espressif/esp-idf) | 乐鑫 SoC 官方物联网开发框架。 | — | 官方简介 |
| [esp-idf-cxx](https://github.com/espressif/esp-idf-cxx) | ESP-IDF 组件的 C++ 封装类。 | — | 官方简介 |
| [esp-iot-solution](https://github.com/espressif/esp-iot-solution) | 乐鑫 IoT 驱动、文档和场景方案集合。 | — | 官方简介 |
| [esp-lowcode-matter](https://github.com/espressif/esp-lowcode-matter) | 构建 Matter 产品的低代码开发框架。 | — | 官方简介 |
| [esp-matter](https://github.com/espressif/esp-matter) | 乐鑫 Matter 产品开发 SDK。 | — | 官方简介 |
| [esp-matter-tools](https://github.com/espressif/esp-matter-tools) | Matter 产品制造与数据模型相关工具。 | — | 官方简介 |
| [esp-privilege-separation](https://github.com/espressif/esp-privilege-separation) | ESP 固件权限隔离框架。 | — | 官方简介 |
| [esp-rainmaker](https://github.com/espressif/esp-rainmaker) | ESP RainMaker 云端控制方案的设备端 Agent。 | — | 官方简介 |
| [esp-rainmaker-common](https://github.com/espressif/esp-rainmaker-common) | RainMaker 多项目共用组件。 | — | 官方简介 |
| [esp-rainmaker-neo-firmware](https://github.com/espressif/esp-rainmaker-neo-firmware) | RainMaker Neo 的固件 SDK。 | — | 官方简介 |
| [esp-swift](https://github.com/espressif/esp-swift) | 在 ESP-IDF 工程中集成 Swift 代码的组件。 | — | 官方简介 |
| [esp-thread-br](https://github.com/espressif/esp-thread-br) | 乐鑫 Thread 边界路由器 SDK。 | — | 官方简介 |
| [esp-vision](https://github.com/espressif/esp-vision) | 面向边缘视觉 AI 的低代码框架。 | — | 官方简介 |
| [esp-wasmachine](https://github.com/espressif/esp-wasmachine) | 可在 ESP 设备上运行 WASM 应用的运行时。 | — | 官方简介 |
| [esp-wdf](https://github.com/espressif/esp-wdf) | 乐鑫 WASM 应用开发框架。 | — | 官方简介 |
| [esp-zerocode-blocks](https://github.com/espressif/esp-zerocode-blocks) | ESP ZeroCode 项目所用的积木式构建块；官方简介只给出名称。 | — | 官方简介 |
| [esp-zigbee-sdk](https://github.com/espressif/esp-zigbee-sdk) | 面向 ESP 芯片的 Zigbee 产品开发 SDK。 | — | 官方简介 |
| [ESP31_RTOS_SDK](https://github.com/espressif/ESP31_RTOS_SDK) | 早期 ESP31B 的 FreeRTOS SDK，已归档。 | 归档 | 官方简介 |
| [ESP8266_NONOS_SDK](https://github.com/espressif/ESP8266_NONOS_SDK) | ESP8266 无操作系统 SDK。 | — | 官方简介 |
| [ESP8266_RTOS_SDK](https://github.com/espressif/ESP8266_RTOS_SDK) | 采用 FreeRTOS 且风格接近 ESP-IDF 的 ESP8266 SDK。 | — | 官方简介 |


### 无线通信与网络协议（30）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [aws-iot-device-sdk-embedded-C](https://github.com/espressif/aws-iot-device-sdk-embedded-C) | 面向 AWS IoT 的嵌入式 C SDK 乐鑫适配分叉。 | Fork | 官方简介 |
| [connectedhomeip](https://github.com/espressif/connectedhomeip) | Matter/Project CHIP 上游仓库的乐鑫分叉。 | Fork | 官方简介 |
| [esp-at](https://github.com/espressif/esp-at) | ESP32 与 ESP8266 等芯片的 AT 指令固件。 | — | 官方简介 |
| [esp-ble-mesh-lib](https://github.com/espressif/esp-ble-mesh-lib) | BLE Mesh 1.1 协议栈预编译库。 | — | 官方简介 |
| [esp-btdm-linux-drv](https://github.com/espressif/esp-btdm-linux-drv) | ESP 芯片在 Linux 上使用的双模蓝牙驱动。 | — | 官方简介 |
| [esp-coex-lib](https://github.com/espressif/esp-coex-lib) | Wi-Fi、蓝牙与 802.15.4 共存机制的预编译库。 | — | 根 README |
| [esp-csi](https://github.com/espressif/esp-csi) | 利用 Wi-Fi CSI 做定位、人体感知等应用的项目。 | — | 官方简介 |
| [esp-extconn](https://github.com/espressif/esp-extconn) | 为缺少内置无线功能的 ESP 芯片提供外部 Wi-Fi/蓝牙连接组件。 | — | 根 README |
| [esp-freertos-coremqtt](https://github.com/espressif/esp-freertos-coremqtt) | 基于 coreMQTT 的 ESP32 MQTT 客户端及示例。 | — | 根 README |
| [esp-hosted](https://github.com/espressif/esp-hosted) | 让 ESP SoC 作为 Linux 或 MCU 主机 Wi-Fi/蓝牙协处理器的方案。 | — | 官方简介 |
| [esp-hosted-linux](https://github.com/espressif/esp-hosted-linux) | ESP-Hosted 对应的 Linux 驱动和芯片端固件。 | — | 官方简介 |
| [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) | ESP-Hosted 的 MCU 主机侧实现。 | — | 根 README |
| [esp-ieee802154-lib](https://github.com/espressif/esp-ieee802154-lib) | IEEE 802.15.4 射频协议栈预编译库。 | — | 官方简介 |
| [esp-iot-bridge](https://github.com/espressif/esp-iot-bridge) | 使 ESP 与其他 MCU 或智能设备共享互联网连接的桥接方案。 | — | 官方简介 |
| [esp-lwip](https://github.com/espressif/esp-lwip) | 带 ESP-IDF 专有补丁的 lwIP 网络协议栈分叉。 | — | 官方简介 |
| [esp-mdf](https://github.com/espressif/esp-mdf) | 旧版 ESP Mesh 开发框架，已归档，官方推荐 esp-mesh-lite。 | 归档 | 官方简介 |
| [esp-mesh-lite](https://github.com/espressif/esp-mesh-lite) | 各节点具备 IP 访问能力的轻量 Wi-Fi Mesh 方案。 | — | 官方简介 |
| [esp-modbus](https://github.com/espressif/esp-modbus) | 支持串口 RS485 和 TCP 的官方 Modbus 组件。 | — | 官方简介 |
| [esp-mqtt](https://github.com/espressif/esp-mqtt) | ESP32 的 MQTT 协议客户端组件。 | — | 官方简介 |
| [esp-nimble](https://github.com/espressif/esp-nimble) | 供 ESP32 与 ESP-IDF 使用的 NimBLE 协议栈分叉。 | Fork | 官方简介 |
| [esp-now](https://github.com/espressif/esp-now) | 无需建立连接的 Wi-Fi 设备间通信协议及示例。 | — | 官方简介 |
| [esp-protocols](https://github.com/espressif/esp-protocols) | ESP-IDF 网络协议相关组件集合。 | — | 官方简介 |
| [esp-thread-lib](https://github.com/espressif/esp-thread-lib) | 供 ESP-IDF 使用的 Thread 协议预编译库。 | — | 官方简介 |
| [esp-wifi-apps](https://github.com/espressif/esp-wifi-apps) | ESP-IDF 的 Wi-Fi 中间件应用集合。 | — | 官方简介 |
| [esp-wifi-drv](https://github.com/espressif/esp-wifi-drv) | ESP 芯片 Linux Wi-Fi 驱动。 | — | 官方简介 |
| [esp-wifi-remote](https://github.com/espressif/esp-wifi-remote) | 让无原生 Wi-Fi 的主控通过传输接口使用远端 ESP Wi-Fi 能力。 | — | 官方简介 |
| [esp-wireless-drivers-3rdparty](https://github.com/espressif/esp-wireless-drivers-3rdparty) | 供第三方代码仓集成的 Wi-Fi/蓝牙驱动包，官方标注进行中。 | — | 官方简介 |
| [esp-zboss-lib](https://github.com/espressif/esp-zboss-lib) | ZBOSS Zigbee 3.0 协议栈预编译库。 | — | 根 README |
| [ESP8266_AT](https://github.com/espressif/ESP8266_AT) | 旧 ESP8266 AT 固件仓库，已归档，官方改用 esp-at。 | 归档 | 官方简介 |
| [openthread](https://github.com/espressif/openthread) | 维护 ESP 特定补丁和发布分支的 OpenThread 分叉。 | Fork | 官方简介 |


### 安全、启动与证书（15）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [esp-bootloader-plus](https://github.com/espressif/esp-bootloader-plus) | 支持压缩及差分压缩升级的增强 Bootloader。 | — | 官方简介 |
| [esp-cryptoauthlib](https://github.com/espressif/esp-cryptoauthlib) | Microchip cryptoauthlib 的发布用分叉。 | — | 官方简介 |
| [esp-idf-sbom](https://github.com/espressif/esp-idf-sbom) | 生成 ESP-IDF 软件物料清单的工具。 | — | 官方简介 |
| [esp-idf-sbom-action](https://github.com/espressif/esp-idf-sbom-action) | 对组件清单执行漏洞扫描的 GitHub Action。 | — | 根 README |
| [esp-idf-security-dashboard](https://github.com/espressif/esp-idf-security-dashboard) | 展示 ESP-IDF 安全漏洞信息的仪表板。 | — | 官方简介 |
| [esp-nuttx-bootloader](https://github.com/espressif/esp-nuttx-bootloader) | 供 ESP NuttX 用户使用的二级 Bootloader 与分区二进制，已归档。 | 归档 | 官方简介 |
| [esp-nvd-mirror](https://github.com/espressif/esp-nvd-mirror) | 从美国 NVD 定期同步的 CVE 与 CPE JSON 数据镜像。 | — | 根 README |
| [esp-pqc](https://github.com/espressif/esp-pqc) | 在 ESP32 上尝试后量子密码学的案例。 | — | 官方简介 |
| [esp-product-security](https://github.com/espressif/esp-product-security) | ESP 产品安全功能与实践文档。 | — | 官方简介 |
| [esp-self-reflasher](https://github.com/espressif/esp-self-reflasher) | 允许设备对自身执行完整 Flash 重刷的组件或应用。 | — | 官方简介 |
| [esp-wolfssl](https://github.com/espressif/esp-wolfssl) | wolfSSL 在 ESP-IDF 和 ESP8266_RTOS_SDK 上的移植。 | — | 官方简介 |
| [esp_secure_cert_mgr](https://github.com/espressif/esp_secure_cert_mgr) | ESP 安全证书管理组件。 | — | 官方简介 |
| [liboqs](https://github.com/espressif/liboqs) | 后量子密码算法实验库的分叉。 | Fork | 官方简介 |
| [mbedtls](https://github.com/espressif/mbedtls) | TLS 加密库 mbedTLS 的乐鑫分叉。 | Fork | 官方简介 |
| [TF-PSA-Crypto](https://github.com/espressif/TF-PSA-Crypto) | PSA Cryptography API 参考实现的分叉。 | Fork | 官方简介 |


### 芯片底层、板级与硬件（34）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [esp-bist](https://github.com/espressif/esp-bist) | 乐鑫芯片内建自测相关库。 | — | 官方简介 |
| [esp-board-manager](https://github.com/espressif/esp-board-manager) | 用 YAML 描述主控与外设并生成板级配置代码的工具。 | — | 根 README |
| [esp-dev-kits](https://github.com/espressif/esp-dev-kits) | ESP 开发板的文档、原理图与出厂固件。 | — | 官方简介 |
| [esp-eth-drivers](https://github.com/espressif/esp-eth-drivers) | ESP-IDF 额外以太网驱动组件集合。 | — | 官方简介 |
| [esp-flash-drivers](https://github.com/espressif/esp-flash-drivers) | 第三方 Flash 芯片驱动仓库。 | — | 官方简介 |
| [esp-hal-3rdparty](https://github.com/espressif/esp-hal-3rdparty) | 同步给 NuttX、Zephyr 等第三方框架使用的乐鑫 HAL 组件。 | — | 根 README |
| [esp-hal-components](https://github.com/espressif/esp-hal-components) | 乐鑫芯片的硬件抽象层组件集合。 | — | 官方简介 |
| [esp-hardware-design-guidelines](https://github.com/espressif/esp-hardware-design-guidelines) | SoC 和模块产品集成的硬件设计指南。 | — | 官方简介 |
| [esp-linux-bsp](https://github.com/espressif/esp-linux-bsp) | ESP32-S31 的 Linux BSP，目前 README 标为开发预览。 | — | 根 README |
| [esp-phy-lib](https://github.com/espressif/esp-phy-lib) | ESP 芯片底层射频功能的预编译库。 | — | 官方简介 |
| [esp-prog-2](https://github.com/espressif/esp-prog-2) | ESP-Prog-2 下载与调试硬件的设计资料。 | — | 根 README |
| [esp-rom-elfs](https://github.com/espressif/esp-rom-elfs) | 乐鑫芯片 ROM 的 ELF/二进制资料。 | — | 官方简介 |
| [esp-twai-components](https://github.com/espressif/esp-twai-components) | ESP-IDF TWAI/CAN 兼容总线组件。 | — | 官方简介 |
| [esp-usb](https://github.com/espressif/esp-usb) | ESP-IDF USB Host 与 Device 相关组件集合。 | — | 根 README |
| [esp-usb-bridge](https://github.com/espressif/esp-usb-bridge) | 用 ESP32-S2/S3 实现 USB 转 UART/JTAG 的桥接固件。 | — | 官方简介 |
| [esp-win-usb-drivers](https://github.com/espressif/esp-win-usb-drivers) | 乐鑫开发板在 Windows 上使用的 USB 驱动。 | — | 根 README |
| [esp32-bt-lib](https://github.com/espressif/esp32-bt-lib) | ESP32 HCI 以下蓝牙协议栈的预编译库。 | — | 官方简介 |
| [esp32-wifi-lib](https://github.com/espressif/esp32-wifi-lib) | ESP32 Wi-Fi 协议栈预编译库。 | — | 官方简介 |
| [esp32c2-bt-lib](https://github.com/espressif/esp32c2-bt-lib) | ESP32-C2 蓝牙协议栈预编译库；README 未说明更细范围，按名称推断。 | — | 推断/待核 |
| [esp32c3-bt-lib](https://github.com/espressif/esp32c3-bt-lib) | ESP32-C3/S3 HCI 以下蓝牙协议栈预编译库。 | — | 官方简介 |
| [esp32c5-bt-lib](https://github.com/espressif/esp32c5-bt-lib) | ESP32-C5 蓝牙协议栈预编译库；README 未说明更细范围，按名称推断。 | — | 推断/待核 |
| [esp32c6-bt-lib](https://github.com/espressif/esp32c6-bt-lib) | ESP32-C6 HCI 以下蓝牙协议栈预编译库。 | — | 官方简介 |
| [esp32c61-bt-lib](https://github.com/espressif/esp32c61-bt-lib) | ESP32-C61 蓝牙协议栈预编译库；官方简介仅列仓库名。 | — | 官方简介 |
| [esp32e22-fw](https://github.com/espressif/esp32e22-fw) | ESP32-E22 统一固件。 | — | 官方简介 |
| [esp32e22-linux-driver](https://github.com/espressif/esp32e22-linux-driver) | ESP32-E22 的 Linux 驱动集合。 | — | 官方简介 |
| [esp32h2-bt-lib](https://github.com/espressif/esp32h2-bt-lib) | ESP32-H2 HCI 以下蓝牙协议栈预编译库。 | — | 官方简介 |
| [esp32h4-bt-lib](https://github.com/espressif/esp32h4-bt-lib) | ESP32-H4 蓝牙协议栈预编译库；README 未说明更细范围，按名称推断。 | — | 推断/待核 |
| [esp32s31-bt-lib](https://github.com/espressif/esp32s31-bt-lib) | ESP32-S31 蓝牙协议栈预编译库；README 未说明更细范围，按名称推断。 | — | 推断/待核 |
| [esp_jrnl](https://github.com/espressif/esp_jrnl) | ESP-IDF 文件系统日志或事务记录组件。 | — | 官方简介 |
| [idf-extra-components](https://github.com/espressif/idf-extra-components) | 乐鑫维护的额外 ESP-IDF 组件集合。 | — | 官方简介 |
| [idf-flash-vendor-patches](https://github.com/espressif/idf-flash-vendor-patches) | Flash 厂商差异补丁的存放仓库。 | — | 官方简介 |
| [kicad-libraries](https://github.com/espressif/kicad-libraries) | 乐鑫芯片、模块和开发板的 KiCad 元器件库。 | — | 官方简介 |
| [svd](https://github.com/espressif/svd) | 乐鑫设备 SVD 外设寄存器描述文件。 | — | 官方简介 |
| [usb-pids](https://github.com/espressif/usb-pids) | 乐鑫 USB VID 下分配给客户的 PID 登记。 | — | 官方简介 |


### 烧录、调试与测试（35）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [esp-ble-tools](https://github.com/espressif/esp-ble-tools) | 主机侧 BLE 调试、验证与日志抓取工具集合。 | — | 根 README |
| [esp-coredump](https://github.com/espressif/esp-coredump) | 检索和分析 ESP-IDF Core Dump 的 Python 工具。 | — | 根 README |
| [esp-debug-adapter](https://github.com/espressif/esp-debug-adapter) | 基于 GDB 的 Debug Adapter Protocol 服务端。 | — | 官方简介 |
| [esp-debug-backend](https://github.com/espressif/esp-debug-backend) | 乐鑫 Python 调试后端。 | — | 官方简介 |
| [esp-emulator](https://github.com/espressif/esp-emulator) | ESP32 系列 SoC 模拟器。 | — | 官方简介 |
| [esp-flasher-stub](https://github.com/espressif/esp-flasher-stub) | 供 esptool 与 esp-serial-flasher 加速烧录的小型芯片端程序。 | — | 根 README |
| [esp-gcov](https://github.com/espressif/esp-gcov) | ESP-IDF 固件代码覆盖率采集组件。 | — | 官方简介 |
| [esp-gdbstub](https://github.com/espressif/esp-gdbstub) | 供 ESP8266 调试的 GDB Stub。 | — | 根 README |
| [esp-gpio-tool](https://github.com/espressif/esp-gpio-tool) | 在主机侧辅助检查或操作 ESP GPIO 的工具。 | — | 根 README |
| [esp-idf-diag](https://github.com/espressif/esp-idf-diag) | 收集 ESP-IDF 环境和项目诊断信息的工具。 | — | 根 README |
| [esp-idf-monitor](https://github.com/espressif/esp-idf-monitor) | ESP-IDF 串口交互和日志监控工具。 | — | 根 README |
| [esp-idf-nvs-partition-gen](https://github.com/espressif/esp-idf-nvs-partition-gen) | 生成 NVS 分区镜像的工具。 | — | 官方简介 |
| [esp-idf-panic-decoder](https://github.com/espressif/esp-idf-panic-decoder) | 解析 Panic 寄存器与栈输出并通过 GDB 展示的工具。 | — | 根 README |
| [esp-idf-provisioning-android](https://github.com/espressif/esp-idf-provisioning-android) | Android 平台的 ESP-IDF 统一配网客户端。 | — | 官方简介 |
| [esp-idf-provisioning-ios](https://github.com/espressif/esp-idf-provisioning-ios) | iOS 平台的 ESP 设备配网 Swift 库。 | — | 根 README |
| [esp-idf-size](https://github.com/espressif/esp-idf-size) | 分析 ESP-IDF 固件大小和内存占用的工具。 | — | 根 README |
| [esp-idf-size-test](https://github.com/espressif/esp-idf-size-test) | esp-idf-size 工具测试时存放构建制品的仓库。 | — | 根 README |
| [esp-riscv-trace](https://github.com/espressif/esp-riscv-trace) | RISC-V 执行追踪相关项目；README 仅有标题，具体用途待核实。 | — | 推断/待核 |
| [esp-serial-flasher](https://github.com/espressif/esp-serial-flasher) | 由其他 MCU 为 ESP SoC 烧录固件的库。 | — | 官方简介 |
| [esp-stub-lib](https://github.com/espressif/esp-stub-lib) | 创建 ESP 芯片端 Flash 烧录 Stub 的 C 库。 | — | 根 README |
| [esp-sysview](https://github.com/espressif/esp-sysview) | 采集 SEGGER SystemView 兼容 Trace 的组件。 | — | 官方简介 |
| [esp-test-tools](https://github.com/espressif/esp-test-tools) | 射频测试等 ESP 测试工具的文档和资源。 | — | 官方简介 |
| [esp-trace-viewer](https://github.com/espressif/esp-trace-viewer) | 在浏览器中查看 ESP-IDF/Zephyr Trace 的工具。 | — | 官方简介 |
| [esp32c3-direct-boot-example](https://github.com/espressif/esp32c3-direct-boot-example) | 演示 ESP32-C3 指定修订版直接启动特性的工程。 | — | 官方简介 |
| [esptool](https://github.com/espressif/esptool) | 串口烧录、设备配置信息处理与芯片交互命令行。 | — | 官方简介 |
| [esptool-js](https://github.com/espressif/esptool-js) | 在浏览器 WebSerial 中为 ESP 芯片烧录固件的 JavaScript 工具。 | — | 官方简介 |
| [esptool-legacy-flasher-stub](https://github.com/espressif/esptool-legacy-flasher-stub) | 旧版 esptool 烧录 Stub，README 声明已由 esp-flasher-stub 接替。 | — | 根 README |
| [freertos-gdb](https://github.com/espressif/freertos-gdb) | 在 GDB 中查看 FreeRTOS 内核对象的 Python 扩展。 | — | 官方简介 |
| [idf-drivers-gdb](https://github.com/espressif/idf-drivers-gdb) | 在 GDB 中查看 ESP-IDF 驱动对象的 Python 扩展。 | — | 官方简介 |
| [iperf-cmd](https://github.com/espressif/iperf-cmd) | 用于网络吞吐量测试的 ESP-IDF iperf 命令组件。 | — | 官方简介 |
| [openocd-esp32](https://github.com/espressif/openocd-esp32) | 支持 ESP32 JTAG 调试的 OpenOCD 分支。 | — | 官方简介 |
| [openocd-on-esp32](https://github.com/espressif/openocd-on-esp32) | 在 ESP32-S3 芯片上运行的 OpenOCD 移植。 | — | 官方简介 |
| [pytest-embedded](https://github.com/espressif/pytest-embedded) | 用于嵌入式设备测试的 pytest 插件。 | — | 官方简介 |
| [pytest-ignore-test-results](https://github.com/espressif/pytest-ignore-test-results) | 按指定规则忽略部分测试结果并维持执行的 pytest 插件。 | — | 根 README |
| [qemu](https://github.com/espressif/qemu) | 带乐鑫设备模拟补丁的 QEMU 分叉。 | Fork | 官方简介 |


### 开发环境、构建与 CI（57）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [actions-internal-test](https://github.com/espressif/actions-internal-test) | 用于验证乐鑫内部 GitHub Actions 的测试仓库。 | — | 官方简介 |
| [astyle_py](https://github.com/espressif/astyle_py) | Astyle 代码格式化器的 Python 封装与 pre-commit 钩子。 | Fork | 官方简介 |
| [build-and-test-esp-idf-projects-example](https://github.com/espressif/build-and-test-esp-idf-projects-example) | ESP-IDF 项目构建与测试 Action 的示例仓库；缺少简介及根 README，按名称推断。 | — | 推断/待核 |
| [build-esp-idf-projects-action](https://github.com/espressif/build-esp-idf-projects-action) | 在同一仓库中批量构建多个 ESP-IDF 项目的 GitHub Action。 | — | 根 README |
| [check-copyright](https://github.com/espressif/check-copyright) | 检查并补写源码许可证 SPDX 头部的工具。 | — | 官方简介 |
| [clang-tidy-runner](https://github.com/espressif/clang-tidy-runner) | 在 ESP-IDF 代码目录运行 clang-tidy 的 Python 工具。 | — | 根 README |
| [conventional-precommit-linter](https://github.com/espressif/conventional-precommit-linter) | 检查 Conventional Commits 提交格式的 pre-commit 钩子。 | — | 官方简介 |
| [cz-plugin-espressif](https://github.com/espressif/cz-plugin-espressif) | 符合乐鑫提交风格的 Commitizen 插件。 | — | 官方简介 |
| [dependency-driven-ci-action](https://github.com/espressif/dependency-driven-ci-action) | 按变更文件决定 ESP-IDF 构建与测试范围的 GitHub Action。 | — | 官方简介 |
| [docker-hub-issue-test](https://github.com/espressif/docker-hub-issue-test) | Docker Hub 问题复现或测试仓库；README 未给出更具体用途。 | — | 根 README |
| [docs-bot-action](https://github.com/espressif/docs-bot-action) | 在乐鑫组织内用聊天机器人知识回复 GitHub Issue 的实验性 Action。 | — | 根 README |
| [esp-bool-parser](https://github.com/espressif/esp-bool-parser) | 解析含芯片能力及环境变量条件的布尔表达式的小工具。 | — | 官方简介 |
| [esp-idf-ci-action](https://github.com/espressif/esp-idf-ci-action) | 运行 ESP-IDF 项目持续集成的 GitHub Action 分叉。 | Fork | 官方简介 |
| [esp-idf-configdep](https://github.com/espressif/esp-idf-configdep) | 优化 sdkconfig.h 依赖以缩小 C 代码重新构建范围的工具。 | — | 根 README |
| [esp-idf-kconfig](https://github.com/espressif/esp-idf-kconfig) | ESP-IDF 的 Kconfig 项目配置实现。 | — | 根 README |
| [esp-pwsh-check](https://github.com/espressif/esp-pwsh-check) | PowerShell 脚本静态检查工具。 | — | 根 README |
| [esp-pylib](https://github.com/espressif/esp-pylib) | 乐鑫 Python 项目共用的日志、工具和常量库。 | — | 官方简介 |
| [esp-toolchain-bin-wrappers](https://github.com/espressif/esp-toolchain-bin-wrappers) | 工具链可执行程序包装器；无简介与根 README，按名称推断。 | — | 推断/待核 |
| [esp-workbench](https://github.com/espressif/esp-workbench) | 维护 ESP32 本地开发环境的工具。 | — | 官方简介 |
| [esp32-arduino-lib-builder](https://github.com/espressif/esp32-arduino-lib-builder) | 生成 arduino-esp32 预编译依赖库的构建脚本。 | — | 根 README |
| [esp32-arduino-libs](https://github.com/espressif/esp32-arduino-libs) | Arduino ESP32 v3 及以上使用的 ESP-IDF 预编译库。 | — | 官方简介 |
| [gh-esp-test-template](https://github.com/espressif/gh-esp-test-template) | ESP 项目 CI 测试模板与演示。 | — | 官方简介 |
| [git-mirror-server](https://github.com/espressif/git-mirror-server) | 托管 Git 仓库镜像的第三方项目分叉。 | Fork | 官方简介 |
| [github-actions](https://github.com/espressif/github-actions) | 乐鑫旧 GitHub Actions 集合，已归档。 | 归档 | 官方简介 |
| [github-esp-dockerfiles](https://github.com/espressif/github-esp-dockerfiles) | 乐鑫 GitHub 自托管 Runner 的 Dockerfile。 | — | 官方简介 |
| [homebrew-eim](https://github.com/espressif/homebrew-eim) | 通过 Homebrew 分发 ESP-IDF Installation Manager 的配方。 | — | 官方简介 |
| [idf-build-apps](https://github.com/espressif/idf-build-apps) | 在 CI 中批量构建 ESP-IDF 应用的工具。 | — | 官方简介 |
| [idf-ci](https://github.com/espressif/idf-ci) | 面向 GitHub Actions 和 GitLab CI 的 ESP-IDF 项目 CI 工具。 | — | 官方简介 |
| [idf-clion-plugin](https://github.com/espressif/idf-clion-plugin) | 在 JetBrains CLion 中使用 ESP-IDF 的插件。 | — | 官方简介 |
| [idf-component-manager](https://github.com/espressif/idf-component-manager) | 解析、下载和管理 ESP-IDF 组件依赖的工具。 | — | 官方简介 |
| [idf-eclipse-plugin](https://github.com/espressif/idf-eclipse-plugin) | ESP-IDF CMake 项目的 Eclipse 乐鑫 IDE 插件。 | — | 官方简介 |
| [idf-env](https://github.com/espressif/idf-env) | 安装与管理 ESP-IDF 多版本本地环境的工具。 | — | 官方简介 |
| [idf-examples-launchpad-ci-action](https://github.com/espressif/idf-examples-launchpad-ci-action) | 为 ESP Launchpad 构建示例的 GitHub Action。 | — | 官方简介 |
| [idf-im-cli](https://github.com/espressif/idf-im-cli) | 旧 ESP-IDF Installation Manager 命令行，已归档。 | 归档 | 官方简介 |
| [idf-im-lib](https://github.com/espressif/idf-im-lib) | ESP-IDF Installation Manager 的共享库。 | — | 官方简介 |
| [idf-im-ui](https://github.com/espressif/idf-im-ui) | ESP-IDF Installation Manager 的图形界面。 | — | 官方简介 |
| [idf-installer](https://github.com/espressif/idf-installer) | ESP-IDF Windows 安装器。 | — | 官方简介 |
| [idf-python](https://github.com/espressif/idf-python) | 含 pip 与虚拟环境的 ESP-IDF 最小独立 Python 发行包。 | — | 官方简介 |
| [idf-python-wheels](https://github.com/espressif/idf-python-wheels) | 为 ESP-IDF 离线和在线安装构建 Python wheel 包。 | — | 官方简介 |
| [idf-web-ide](https://github.com/espressif/idf-web-ide) | 基于 Eclipse Theia 的 ESP-IDF 云端或桌面 IDE。 | — | 官方简介 |
| [idf_py_exe_tool](https://github.com/espressif/idf_py_exe_tool) | 在 Windows 上调用 idf.py 的可执行包装器。 | — | 官方简介 |
| [inno-download-plugin](https://github.com/espressif/inno-download-plugin) | Inno Setup 下载插件的第三方镜像分叉。 | Fork | 官方简介 |
| [innosetup-cmdlinerunner](https://github.com/espressif/innosetup-cmdlinerunner) | 在 Inno Setup 安装器中执行命令并读取输出的扩展。 | — | 官方简介 |
| [install-esp-idf-action](https://github.com/espressif/install-esp-idf-action) | 在 GitHub Actions Runner 上安装 ESP-IDF 的 Action。 | — | 官方简介 |
| [iwidc](https://github.com/espressif/iwidc) | ESP-IDF Web IDE 的桌面辅助程序。 | — | 官方简介 |
| [python-binary-action](https://github.com/espressif/python-binary-action) | 跨平台用 PyInstaller 构建 Python 独立可执行程序的 Action。 | — | 根 README |
| [release-sign](https://github.com/espressif/release-sign) | 乐鑫发布制品跨平台签名的复合 GitHub Action。 | — | 根 README |
| [release-zips-action](https://github.com/espressif/release-zips-action) | 创建带子模块完整源码发布 ZIP 的 GitHub Action。 | — | 官方简介 |
| [shared-github-dangerjs](https://github.com/espressif/shared-github-dangerjs) | 乐鑫 GitHub 项目复用的 DangerJS CI 工作流。 | — | 官方简介 |
| [sync-jira-actions](https://github.com/espressif/sync-jira-actions) | 把 GitHub 项目数据同步到乐鑫 Jira 的 Action。 | — | 官方简介 |
| [sync-pr-to-gitlab](https://github.com/espressif/sync-pr-to-gitlab) | 将获批 GitHub PR 同步至内部 GitLab 的 Action。 | — | 官方简介 |
| [test-esp-idf-projects-action](https://github.com/espressif/test-esp-idf-projects-action) | ESP-IDF 项目测试 GitHub Action；无简介与根 README，按名称推断。 | — | 推断/待核 |
| [test-project-bot](https://github.com/espressif/test-project-bot) | 测试项目自动配置的机器人仓库。 | — | 根 README |
| [upload-components-ci-action](https://github.com/espressif/upload-components-ci-action) | 把 ESP-IDF 组件上传到组件注册表的 Action。 | — | 官方简介 |
| [vscode-esp-idf-extension](https://github.com/espressif/vscode-esp-idf-extension) | VS Code 的 ESP-IDF 开发扩展。 | — | 官方简介 |
| [vscode-esp-idf-web-extension](https://github.com/espressif/vscode-esp-idf-web-extension) | VS Code Web 环境的 ESP-IDF 扩展。 | — | 官方简介 |
| [vscode-extension-codespace-test](https://github.com/espressif/vscode-extension-codespace-test) | ESP-IDF 在 GitHub Codespaces 使用的模板与测试仓库。 | — | 官方简介 |


### 工具链、系统移植与第三方依赖（33）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [asio](https://github.com/espressif/asio) | Asio C++ 异步 I/O 库分叉，现已归档。 | Fork、归档 | 官方简介 |
| [binutils-esp32ulp](https://github.com/espressif/binutils-esp32ulp) | 支持 ESP32 ULP 协处理器的 Binutils 工具链分叉。 | — | 官方简介 |
| [binutils-gdb](https://github.com/espressif/binutils-gdb) | GNU Binutils 与 GDB 源码镜像分叉。 | Fork | 官方简介 |
| [cJSON](https://github.com/espressif/cJSON) | 轻量级 C 语言 JSON 解析库分叉。 | Fork | 官方简介 |
| [clang-xtensa](https://github.com/espressif/clang-xtensa) | 旧 Xtensa Clang 分叉，已归档，官方指向 llvm-project。 | Fork、归档 | 官方简介 |
| [crosstool-NG](https://github.com/espressif/crosstool-NG) | 加入 Xtensa 支持的 crosstool-NG 工具链构建分叉。 | Fork | 官方简介 |
| [esp-boost](https://github.com/espressif/esp-boost) | 移植到 ESP SoC 的 Boost C++ 库。 | — | 官方简介 |
| [esp-buildroot-external](https://github.com/espressif/esp-buildroot-external) | ESP32-S31 的 Buildroot 外部树，目前 README 标为开发预览。 | — | 根 README |
| [esp-llvm-embedded-toolchain](https://github.com/espressif/esp-llvm-embedded-toolchain) | 构建 LLVM 嵌入式工具链的脚本和工具。 | — | 官方简介 |
| [esp-xtensaconfig-lib](https://github.com/espressif/esp-xtensaconfig-lib) | 供 GCC/Binutils/GDB 在运行时加载 Xtensa 配置的插件分叉。 | Fork | 官方简介 |
| [gcc](https://github.com/espressif/gcc) | GNU GCC 编译器的乐鑫分叉。 | Fork | 根 README |
| [glibc](https://github.com/espressif/glibc) | GNU C Library 的乐鑫分叉。 | — | 根 README |
| [json_generator](https://github.com/espressif/json_generator) | 支持流式输出的轻量 C JSON 生成器分叉。 | Fork | 官方简介 |
| [json_parser](https://github.com/espressif/json_parser) | 基于 JSMN 的 JSON 解析器分叉。 | Fork | 官方简介 |
| [kconfig-frontends](https://github.com/espressif/kconfig-frontends) | ESP-IDF 使用的 Kconfig 前端旧分叉，已归档。 | 归档 | 官方简介 |
| [linux](https://github.com/espressif/linux) | 带乐鑫芯片补丁的 Linux 内核分叉。 | Fork | 官方简介 |
| [llvm-project](https://github.com/espressif/llvm-project) | 带 Xtensa 支持补丁的 LLVM 分叉。 | Fork | 官方简介 |
| [llvm-xtensa](https://github.com/espressif/llvm-xtensa) | 旧 LLVM Xtensa 分叉，已归档，官方指向 llvm-project。 | Fork、归档 | 官方简介 |
| [meta-espressif](https://github.com/espressif/meta-espressif) | Yocto/OpenEmbedded 的乐鑫芯片构建层。 | — | 官方简介 |
| [musl](https://github.com/espressif/musl) | musl C 标准库的乐鑫分叉。 | — | 根 README |
| [newlib-esp32](https://github.com/espressif/newlib-esp32) | ESP32 ROM 与 ESP-IDF 使用的 newlib C 标准库。 | — | 官方简介 |
| [no_std-training-test](https://github.com/espressif/no_std-training-test) | Rust 在 ESP 芯片上进行 no_std 裸机开发的入门资料分叉。 | Fork | 官方简介 |
| [opensbi](https://github.com/espressif/opensbi) | 带乐鑫补丁的 RISC-V OpenSBI 分叉。 | Fork | 官方简介 |
| [picolibc](https://github.com/espressif/picolibc) | 面向小内存嵌入式系统的 C 标准库分叉。 | — | 根 README |
| [rust-esp32-example](https://github.com/espressif/rust-esp32-example) | 在 ESP-IDF 工程内集成 Rust 的示例。 | — | 官方简介 |
| [tinyusb](https://github.com/espressif/tinyusb) | 带乐鑫专用补丁的 TinyUSB 分叉。 | Fork | 官方简介 |
| [tlsf](https://github.com/espressif/tlsf) | ESP-IDF 使用的 TLSF 内存分配器补丁分叉。 | Fork | 官方简介 |
| [u-boot](https://github.com/espressif/u-boot) | 带乐鑫补丁的 U-Boot 引导程序分叉。 | Fork | 官方简介 |
| [wasm-micro-runtime](https://github.com/espressif/wasm-micro-runtime) | WebAssembly Micro Runtime 的乐鑫分叉。 | Fork | 官方简介 |
| [xtensa-dynconfig](https://github.com/espressif/xtensa-dynconfig) | GNU Xtensa 工具链的动态 CPU 配置插件生成器。 | — | 官方简介 |
| [xtensa-isa-doc](https://github.com/espressif/xtensa-isa-doc) | 乐鑫汇编的 Xtensa 指令集参考文档。 | — | 根 README |
| [xtensa-overlays](https://github.com/espressif/xtensa-overlays) | 构建 Xtensa GCC/Binutils/GDB/Newlib 时使用的芯片配置覆盖层。 | — | 官方简介 |
| [zephyr-toolchain](https://github.com/espressif/zephyr-toolchain) | 供 Zephyr 中乐鑫芯片使用的工具链。 | — | 官方简介 |


### 云平台、生态与移动端（37）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [aws-quickconnect](https://github.com/espressif/aws-quickconnect) | AWS QuickConnect 连接示例与预编译文件。 | — | 官方简介 |
| [esp-afr-sdk](https://github.com/espressif/esp-afr-sdk) | Amazon FreeRTOS 的乐鑫基础 SDK，已归档。 | 归档 | 官方简介 |
| [esp-ali-smartliving](https://github.com/espressif/esp-ali-smartliving) | 阿里云生活物联网与天猫精灵接入项目，已归档。 | 归档 | 官方简介 |
| [esp-aliro](https://github.com/espressif/esp-aliro) | ESP32 系列 Aliro Reader 开发框架，包含 NFC 接入能力。 | — | 根 README |
| [esp-aliyun](https://github.com/espressif/esp-aliyun) | 阿里云 IoTKit 的 ESP32/ESP8266 移植，已归档。 | 归档 | 官方简介 |
| [esp-apple-homekit-adk](https://github.com/espressif/esp-apple-homekit-adk) | Apple 开源 HomeKit ADK 的 ESP 平台移植。 | — | 官方简介 |
| [esp-aws-expresslink-eval](https://github.com/espressif/esp-aws-expresslink-eval) | AWS IoT ExpressLink 的评估固件与示例。 | — | 官方简介 |
| [esp-aws-iot](https://github.com/espressif/esp-aws-iot) | ESP32 系列使用 AWS IoT 服务的 SDK。 | — | 官方简介 |
| [esp-azure](https://github.com/espressif/esp-azure) | ESP8266/ESP32 连接 Azure IoT 的 SDK。 | — | 官方简介 |
| [esp-baidu-iot](https://github.com/espressif/esp-baidu-iot) | ESP32/ESP8266 接入百度天工物联网平台的适配，已归档。 | 归档 | 官方简介 |
| [esp-cloud-common](https://github.com/espressif/esp-cloud-common) | 乐鑫云产品之间复用的 Go 基础设施模块和代码库。 | — | 根 README |
| [esp-google-iot](https://github.com/espressif/esp-google-iot) | Google Cloud IoT 的 ESP-IDF 组件，已归档。 | 归档 | 官方简介 |
| [esp-homekit-sdk](https://github.com/espressif/esp-homekit-sdk) | 开发 Apple HomeKit 配件的 ESP SDK。 | — | 根 README |
| [esp-insights](https://github.com/espressif/esp-insights) | 联网设备的远程诊断与可观测框架。 | — | 官方简介 |
| [esp-joylink](https://github.com/espressif/esp-joylink) | 京东 Joylink 的 ESP32/ESP8266 接入演示。 | — | 官方简介 |
| [esp-qcloud](https://github.com/espressif/esp-qcloud) | ESP-IDF 原生接入腾讯 IoT Explorer/腾讯连连的方案。 | — | 官方简介 |
| [esp-rainmaker-admin-cli](https://github.com/espressif/esp-rainmaker-admin-cli) | RainMaker 帐号、设备证书和服务配置管理命令行。 | — | 根 README |
| [esp-rainmaker-android](https://github.com/espressif/esp-rainmaker-android) | RainMaker Android 应用源码。 | — | 官方简介 |
| [esp-rainmaker-app-cdf-ts](https://github.com/espressif/esp-rainmaker-app-cdf-ts) | RainMaker TypeScript 应用的基础 CDF 定义。 | — | 官方简介 |
| [esp-rainmaker-app-sdk-ts](https://github.com/espressif/esp-rainmaker-app-sdk-ts) | RainMaker 应用 TypeScript SDK。 | — | 官方简介 |
| [esp-rainmaker-cli](https://github.com/espressif/esp-rainmaker-cli) | RainMaker 服务普通用户操作命令行。 | — | 根 README |
| [esp-rainmaker-custom-development](https://github.com/espressif/esp-rainmaker-custom-development) | RainMaker 自定义开发相关项目；官方简介未列细节。 | — | 官方简介 |
| [esp-rainmaker-home](https://github.com/espressif/esp-rainmaker-home) | RainMaker Home 应用项目。 | — | 官方简介 |
| [esp-rainmaker-ios](https://github.com/espressif/esp-rainmaker-ios) | RainMaker iOS 应用源码。 | — | 官方简介 |
| [esp-rainmaker-mcp](https://github.com/espressif/esp-rainmaker-mcp) | RainMaker MCP 服务器。 | — | 官方简介 |
| [esp-rainmaker-neo](https://github.com/espressif/esp-rainmaker-neo) | RainMaker Neo 云端后端。 | — | 官方简介 |
| [esp-rainmaker-neo-app-sdk-ts](https://github.com/espressif/esp-rainmaker-neo-app-sdk-ts) | RainMaker Neo 应用 TypeScript SDK。 | — | 官方简介 |
| [esp-rainmaker-oauth2-integration](https://github.com/espressif/esp-rainmaker-oauth2-integration) | RainMaker 的 OAuth2 集成。 | — | 官方简介 |
| [esp-rainmaker-webhooks](https://github.com/espressif/esp-rainmaker-webhooks) | RainMaker 的 Webhook 集成项目。 | — | 官方简介 |
| [esp-welink](https://github.com/espressif/esp-welink) | ESP32/ESP8266 接入腾讯微瓴的方案，已归档。 | 归档 | 官方简介 |
| [esp32-alink](https://github.com/espressif/esp32-alink) | ESP32 接入阿里 Alink 的旧组件，已归档。 | 归档 | 官方简介 |
| [esp32-alink-demo](https://github.com/espressif/esp32-alink-demo) | ESP32 Alink 接入演示，已归档。 | 归档 | 官方简介 |
| [esp8266-alink-sds](https://github.com/espressif/esp8266-alink-sds) | ESP8266 Alink SDS 接入旧示例，已归档。 | 归档 | 官方简介 |
| [esp8266-alink-v1.0](https://github.com/espressif/esp8266-alink-v1.0) | ESP8266 Alink 1.0 旧版接入项目，已归档。 | 归档 | 官方简介 |
| [esp8266-dual-cloud](https://github.com/espressif/esp8266-dual-cloud) | ESP8266 同时接入 Alink 和 Joylink 的旧演示，已归档。 | 归档 | 官方简介 |
| [ESP8266_RTOS_ALINK_DEMO](https://github.com/espressif/ESP8266_RTOS_ALINK_DEMO) | 基于早期 Alink 1.0 的 ESP8266 RTOS 演示，已归档。 | 归档 | 官方简介 |
| [esp_weaver](https://github.com/espressif/esp_weaver) | 将 ESP 设备集成进 Home Assistant 的项目。 | — | 官方简介 |


### 音视频、图形与人工智能（42）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [Adafruit-GFX-Library](https://github.com/espressif/Adafruit-GFX-Library) | 添加 ESP32 适配的 Adafruit 图形绘制基础库分叉。 | Fork | 官方简介 |
| [Arduino-FOC](https://github.com/espressif/Arduino-FOC) | Arduino 电机磁场定向控制算法库的分叉。 | Fork | 官方简介 |
| [esp-agents-firmware](https://github.com/espressif/esp-agents-firmware) | ESP Agents 设备侧固件项目；官方简介只给出产品名称。 | — | 官方简介 |
| [esp-apa](https://github.com/espressif/esp-apa) | ESP 芯片边缘音频算法集合，覆盖语音前端与音频增强。 | — | 根 README |
| [esp-audio-dev](https://github.com/espressif/esp-audio-dev) | 为 ESP 应用封装统一硬件访问接口的音频设备组件集合。 | — | 根 README |
| [esp-ble-audio-lib](https://github.com/espressif/esp-ble-audio-lib) | 供 ESP-IDF 使用的蓝牙音频 Profile 预编译库。 | — | 根 README |
| [esp-box](https://github.com/espressif/esp-box) | ESP-BOX AIoT 开发平台的板级应用和示例。 | — | 官方简介 |
| [esp-claw](https://github.com/espressif/esp-claw) | 面向 IoT 设备的聊天编程 AI Agent 框架。 | — | 官方简介 |
| [esp-claw-skills-lab](https://github.com/espressif/esp-claw-skills-lab) | ESP-Claw 技能实验项目；官方简介只给出名称。 | — | 官方简介 |
| [esp-desktop-buddy](https://github.com/espressif/esp-desktop-buddy) | ESP Desktop Buddy 设备端 SDK。 | — | 官方简介 |
| [esp-detection](https://github.com/espressif/esp-detection) | 在 ESP 芯片上运行的 YOLOv11 轻量实时目标检测库。 | — | 官方简介 |
| [esp-dl](https://github.com/espressif/esp-dl) | 面向 AIoT 的乐鑫深度学习库。 | — | 官方简介 |
| [esp-drone](https://github.com/espressif/esp-drone) | ESP32 系列迷你无人机或四旋翼固件。 | — | 官方简介 |
| [esp-dsp](https://github.com/espressif/esp-dsp) | ESP-IDF 的数字信号处理库。 | — | 官方简介 |
| [esp-gsp](https://github.com/espressif/esp-gsp) | 把 JSON 场景和资源预编译为嵌入式 UI Bundle 的图形场景框架。 | — | 根 README |
| [esp-h264-component](https://github.com/espressif/esp-h264-component) | 乐鑫 H.264 编码与解码组件。 | — | 官方简介 |
| [esp-health](https://github.com/espressif/esp-health) | 用于可穿戴和健康设备的传感信号处理与健康指标算法库。 | — | 根 README |
| [esp-moonlight](https://github.com/espressif/esp-moonlight) | Moonlight 游戏串流客户端的 ESP 移植，官方提示有限维护。 | — | 根 README |
| [esp-nn](https://github.com/espressif/esp-nn) | 面向乐鑫芯片优化的神经网络算子。 | — | 官方简介 |
| [esp-opencv-component](https://github.com/espressif/esp-opencv-component) | 打包为 ESP-IDF 组件的 OpenCV。 | — | 官方简介 |
| [esp-port-for-amazon-kvs-sdk](https://github.com/espressif/esp-port-for-amazon-kvs-sdk) | Amazon Kinesis Video Streams WebRTC C SDK 的 ESP 移植。 | — | 官方简介 |
| [esp-ppq](https://github.com/espressif/esp-ppq) | 神经网络模型离线量化工具 PPQ 的分叉。 | Fork | 官方简介 |
| [esp-skainet](https://github.com/espressif/esp-skainet) | 乐鑫智能语音助手方案。 | — | 官方简介 |
| [esp-sr](https://github.com/espressif/esp-sr) | 乐鑫语音识别框架。 | — | 官方简介 |
| [esp-tflite-micro](https://github.com/espressif/esp-tflite-micro) | TensorFlow Lite Micro 的乐鑫芯片移植。 | — | 官方简介 |
| [esp-va-sdk](https://github.com/espressif/esp-va-sdk) | Alexa、Google Assistant 与 Dialogflow 的语音助手 SDK。 | — | 官方简介 |
| [esp-video-components](https://github.com/espressif/esp-video-components) | 摄像头及视频功能相关组件集合。 | — | 官方简介 |
| [esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution) | 构建 WebRTC 音视频应用的 ESP 组件与示例集合。 | — | 根 README |
| [esp-who](https://github.com/espressif/esp-who) | 人脸检测与识别框架。 | — | 官方简介 |
| [esp31-smsemu](https://github.com/espressif/esp31-smsemu) | 将 Sega Master System 模拟器移植到 ESP31 的历史演示，已归档。 | 归档 | 根 README |
| [esp32-camera](https://github.com/espressif/esp32-camera) | ESP32 系列图像传感器驱动与图像处理工具。 | — | 根 README |
| [esp32-doom](https://github.com/espressif/esp32-doom) | ESP32 上运行 PrBoom/Doom 的概念验证。 | — | 官方简介 |
| [esp32-nesemu](https://github.com/espressif/esp32-nesemu) | 在 ESP32 上运行 NES 模拟器的概念验证。 | — | 官方简介 |
| [esp32-quake](https://github.com/espressif/esp32-quake) | 面向 ESP32-P4 评估板的 Quake 移植。 | — | 官方简介 |
| [esp32-scummvm](https://github.com/espressif/esp32-scummvm) | 面向 ESP32-P4 的 ScummVM 游戏引擎移植。 | — | 官方简介 |
| [ESP8266_MP3_DECODER](https://github.com/espressif/ESP8266_MP3_DECODER) | 在 ESP8266 Non-OS SDK 上运行的 MP3 解码演示，已归档。 | 归档 | 官方简介 |
| [libuvc](https://github.com/espressif/libuvc) | 带乐鑫适配补丁的 USB 视频设备库分叉。 | Fork | 官方简介 |
| [opencv](https://github.com/espressif/opencv) | 带乐鑫芯片补丁的 OpenCV 分叉。 | Fork | 官方简介 |
| [opencv_contrib](https://github.com/espressif/opencv_contrib) | 带乐鑫芯片补丁的 OpenCV 附加模块分叉。 | Fork | 官方简介 |
| [qrcode-demo](https://github.com/espressif/qrcode-demo) | 二维码识别演示。 | — | 官方简介 |
| [tensorflow](https://github.com/espressif/tensorflow) | TensorFlow 框架的第三方分叉。 | Fork | 官方简介 |
| [WROVER_KIT_LCD](https://github.com/espressif/WROVER_KIT_LCD) | ESP-WROVER-KIT 显示屏的 Arduino 驱动库分叉。 | Fork | 官方简介 |


### 文档、教程与演示（32）

| 仓库 | 用途 | 状态 | 依据 |
| --- | --- | --- | --- |
| [.github](https://github.com/espressif/.github) | 组织首页、GitHub 公共资料及开发者关系管理仓库。 | — | 官方简介 |
| [blockdiag](https://github.com/espressif/blockdiag) | 根据文本规格生成框图的第三方工具分叉。 | Fork | 根 README |
| [blowfish](https://github.com/espressif/blowfish) | Hugo 网站与博客主题分叉，与 ESP 固件无直接关系。 | Fork | 官方简介 |
| [book-esp32c3-iot-projects](https://github.com/espressif/book-esp32c3-iot-projects) | ESP32-C3 物联网开发书籍的配套代码。 | — | 官方简介 |
| [developer-portal](https://github.com/espressif/developer-portal) | 乐鑫开发者门户站点项目。 | — | 官方简介 |
| [developer-portal-codebase](https://github.com/espressif/developer-portal-codebase) | 开发者门户文章、教程和测试所用的示例代码。 | — | 官方简介 |
| [doxybook](https://github.com/espressif/doxybook) | 把 C/C++ Doxygen API 输出转为单文件 Markdown 的分叉工具。 | Fork | 官方简介 |
| [eclipse-plugin-esp32](https://github.com/espressif/eclipse-plugin-esp32) | 旧版 Eclipse ESP32 插件，已归档，官方指向 idf-eclipse-plugin。 | Fork、归档 | 官方简介 |
| [esp-chip-errata](https://github.com/espressif/esp-chip-errata) | 芯片已知问题与对应解决方法文档。 | — | 官方简介 |
| [esp-docs](https://github.com/espressif/esp-docs) | 乐鑫文档项目共用的 Sphinx Python 扩展包装层。 | — | 官方简介 |
| [esp-docs-mdbook](https://github.com/espressif/esp-docs-mdbook) | 符合乐鑫文档风格的 mdBook 主题。 | — | 根 README |
| [esp-faq](https://github.com/espressif/esp-faq) | 乐鑫常见问题文档。 | — | 根 README |
| [esp-idf-template](https://github.com/espressif/esp-idf-template) | ESP-IDF 旧版应用模板，已归档。 | 归档 | 官方简介 |
| [esp-jumpstart](https://github.com/espressif/esp-jumpstart) | 从原型到量产的 ESP 开发教程与样例。 | — | 官方简介 |
| [esp-launchpad](https://github.com/espressif/esp-launchpad) | 可配置的浏览器端固件烧录页面。 | — | 官方简介 |
| [esp-technical-reference-manual-latex](https://github.com/espressif/esp-technical-reference-manual-latex) | 乐鑫 SoC 技术参考手册的 LaTeX 源码。 | — | 官方简介 |
| [esp-toolchain-docs](https://github.com/espressif/esp-toolchain-docs) | 乐鑫工具链与调试器文档。 | — | 官方简介 |
| [esp32-c3-book-en](https://github.com/espressif/esp32-c3-book-en) | ESP32-C3 英文书籍资料。 | — | 官方简介 |
| [esp32-iotivity](https://github.com/espressif/esp32-iotivity) | ESP32 支持 OCF/OIC IoTivity 的旧指南，已归档。 | 归档 | 官方简介 |
| [esp8266-nonos-sample-code](https://github.com/espressif/esp8266-nonos-sample-code) | ESP8266 Non-OS SDK 示例代码；无简介与根 README，按名称推断。 | 归档 | 推断/待核 |
| [esp8266-rtos-sample-code](https://github.com/espressif/esp8266-rtos-sample-code) | ESP8266 RTOS SDK 示例代码；无简介与根 README，按名称推断。 | 归档 | 推断/待核 |
| [example_components](https://github.com/espressif/example_components) | 供 ESP-IDF 组件管理器演示使用的样例组件仓库。 | — | 根 README |
| [jupyter-lite-micropython](https://github.com/espressif/jupyter-lite-micropython) | JupyterLite 的 MicroPython/CircuitPython 内核。 | — | 官方简介 |
| [maker-faire-cz](https://github.com/espressif/maker-faire-cz) | 乐鑫在 Maker Faire 展示的演示资料。 | — | 官方简介 |
| [matter_data_model_interpreter](https://github.com/espressif/matter_data_model_interpreter) | Matter 数据模型解析与解释工具。 | — | 官方简介 |
| [midi-workshop](https://github.com/espressif/midi-workshop) | ESP32-S3 USB MIDI 工作坊资料。 | — | 官方简介 |
| [network_demo](https://github.com/espressif/network_demo) | 基于 mDNS 的 ESP32 本地自动发现与通信演示。 | — | 根 README |
| [skills](https://github.com/espressif/skills) | 乐鑫产品与框架相关的 AI Agent Skills 集合。 | — | 官方简介 |
| [slidev-esp-template](https://github.com/espressif/slidev-esp-template) | 符合乐鑫风格的 Slidev 演示文稿模板。 | — | 官方简介 |
| [sphinx_idf_theme](https://github.com/espressif/sphinx_idf_theme) | ESP-IDF 文档使用的 Sphinx 主题分叉。 | Fork | 官方简介 |
| [sphinx_selective_exclude](https://github.com/espressif/sphinx_selective_exclude) | 支持 Sphinx 文档条件排除的扩展分叉。 | Fork | 官方简介 |
| [this-month-in-esps](https://github.com/espressif/this-month-in-esps) | 汇集乐鑫社区新闻与发布动态的进行中项目。 | — | 根 README |
