# ESP Base 固件

生产主应用为 `apps/esp_base/main`，原生消息计数直接由控制 owner 执行，复用现有认证 MQTT 消息输入。固件 OTA 独立于业务包，使用 V4 收据、精确签名固件身份、唯一升级 claim、短 Flash I/O 仲裁和本地 30 秒启动确认。设备 FRP 的流式上传与本机 USB 命令触发的 HTTPS URL OTA 共用同一校验、备用槽写入和恢复链。ESP Tool Mac App 的软件已收敛为本机设备管理，内置 FRP／frpc 与远程 Bridge 接线已删除，实际安装／USB调用链仍待验；固件自身的 FRP／MQTT 能力保持。

## 源码与目标

- `components/native_business`：有界消息字节计数、状态、暂停／恢复和 100 ms 定时状态；不新增业务线程或队列。
- `components/device_protocol`：USB／MQTT／FRP 严格协议、身份／boot／期限／请求守卫、原生结果和 OTA owner。
- `components/ota_operation`：182 B V4 固件收据、签名槽观察、写前意图、恢复与长／短存储占用。
- `components/remote_config`、`device_identity`、`wifi_runtime`、`time_runtime`：既有配置与设备事实。

C3 使用 USB Serial/JTAG VFS；ESP32 使用 UART0。控制任务栈 8,192 B，主任务至少 6,144 B。签名 C3 使用 RSA-3072 v2，ESP32 使用 ECDSA P-256 v1；目标、签名方案和几何各自核对，软件测试键不属于正式信任。

[C3 分区](partitions/c3-partition-table.csv)和 [ESP32 分区](partitions/esp32-partition-table.csv)均为双 `0x1e0000` app，移除业务包区。scratch、Base NVS 与 ESP32 旧 AT 区保持各自真实定位；新布局必须通过一次性有线装配迁入。旧 V3／损坏收据不能当作空状态，普通 app OTA 不能迁分区表。

## 构建

先读取 [SDK 与宿主工具](../tools/README.md)，导出锁定 SDK 环境。显式 target、sdkconfig、defaults 与 build 目录，分别解析两目标锁。组件版本变化必须让官方 Component Manager 重新解析实际目标 lock；`update-dependencies` 默认 lock 名不能代替 ESP32 自定义 lock 的核对。

```bash
idf.py -C firmware -B /absolute/build-c3 -DIDF_TARGET=esp32c3 reconfigure
ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_host_tests.sh
```

ESP32 自动签名构建必须使用仓外绝对路径 P-256 键，启用 signed boot/update、build signed binaries 与 rollback。无签名只读软件探针须显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON`，不能作为刷写制品。C3 自动签名同样在独立签名 defaults 提供仓外 RSA 键。正式制品还需官方验签、精确 signed bin 摘要与分区容量核对。

外部签名准备使用官方 `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`，保留 ESP32 的 ECDSA 签名启动与更新、C3 的 RSA 签名更新以及双目标 rollback；两目标硬件 Secure Boot 均关闭。ESP32同时要求仓外绝对路径、非链接普通64B的 `CONFIG_SECURE_BOOT_VERIFICATION_KEY`，直接消费本轮已核对的公开P-256验证字节；不能启用offline probe代替该装配。此路径不读取私有签名键，只产unsigned app／partition输入，须再由受控签名事实源独立签名、验签和核对完整signed身份后才能交付或发送OTA。自动签名 `y` 的原私有键合同保持，未签输入不具有消费资格。

精确公开依赖来自 [唯一组件清单](components/device_protocol/idf_component.yml)与 [C3](dependencies.lock)／[ESP32](dependencies.lock.esp32)锁。host 使用这些已解析组件，不从相邻源仓运行时 import。构建图只读取当前锁定的 MQTT／OTA／FRP 与 cJSON，不读取退役运行组件。

本轮完整输入、失败记录和签名结果见[原生软件检查点](../docs/operations/native_software_checkpoint.md)；实板及正式交付边界见[执行计划](../docs/operations/ota-allocation-diagnostic-checkpoint.md)。

工程结构、工具链、签名制品和设备写入边界遵循[Embedded Firmware 工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded-firmware/embedded-firmware-golden-path.md)。
