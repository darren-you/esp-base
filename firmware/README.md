# ESP Base 固件

构建、签名和刷写步骤见[仓库 README](../README.md)，协议见[设备—云协议](../docs/design/device-protocol.md)。工程结构遵循 [Embedded Firmware 工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded-firmware/embedded-firmware-golden-path.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `apps/esp_base/main` | 基座固件入口：只调用 `base_app_run(NULL)` |
| `components/base_app` | 主程序：控制任务、系统 MQTT、状态文档、命令执行、配置试用与 OTA 编排，以及业务层回调 |
| `components/base_protocol` | 主题、命令解码、应答与去重；纯逻辑，不依赖 ESP-IDF |
| `components/base_ota` | OTA 状态机（纯逻辑）和基于 `esp_https_ota` 的下载任务 |
| `components/base_store` | NVS 中的设备 UUID、Wi-Fi／系统 MQTT／业务 MQTT 配置和 OTA 记录 |
| `components/base_net` | Wi-Fi STA 与 SNTP |
| `partitions/partitions.csv` | 两个目标共用的 4 MiB 分区表 |
| `tests` | host 测试 |

## 运行方式

控制任务就是主任务，阻塞在一个事件队列上。Wi-Fi、SNTP、MQTT 客户端、定时器和 OTA 下载任务都只向队列投递事件，没有事件时控制任务不被唤醒。状态文档里的 `wakeups` 是控制任务处理过的事件数，可用来确认空闲时没有周期性唤醒；唯一的周期事件是每 15 分钟一次的状态刷新。

启动顺序：读取运行固件身份 → 读取 NVS → 连接 Wi-Fi → SNTP 对时 → 连接 Broker。TLS 用 ESP-IDF 自带的根证书包校验证书链和主机名，所以必须先对时。

## 分区与持久数据

```
nvs       0x009000  64 KiB   设备 UUID、配置、OTA 记录、射频校准
otadata   0x019000   8 KiB
phy_init  0x01b000   4 KiB
ota_0     0x020000  1920 KiB
ota_1     0x200000  1920 KiB
coredump  0x3e0000  64 KiB
```

设备 UUID、Wi-Fi 和系统 MQTT 配置在 NVS 的 `base` 命名空间，由 `tools/esp_base.py flash --provision` 生成 NVS 镜像写入；固件出错时不擦除 NVS。系统 MQTT 是 esp-base 连管理 Broker 的那条连接，只能经出厂资料修改。经 `cmd/config` 下发的新 Wi-Fi 先试用，连上 Broker 才保存，120 秒内连不上就恢复旧配置。`cmd/config` 下发的业务 MQTT（`biz_mqtt_uri`、`biz_mqtt_user`、`biz_mqtt_pass`）收到即保存，留给业务层另开连接，由 `base_store_load_config()` 读取；esp-base 自己不连接它。

## 业务固件

业务固件是一个独立的 ESP-IDF 工程，按精确提交引用本仓：把 `firmware/components` 加进 `EXTRA_COMPONENT_DIRS`，`SDKCONFIG_DEFAULTS` 先列本仓的 `sdkconfig.defaults`（目标专属的 `.<target>` 会一并加载）再列自己的，分区表与本仓逐字节相同（正式打包时核对）。它的 `app_main` 调用 `base_app_run(&业务回调)`：

- `init(device_id, config)`：控制任务启动时、联网之前调用一次，用来把输出脚置到安全状态、读取业务参数（`base_store_business_string`，NVS 的 `business` 命名空间）。
- `connect(business_mqtt)`：系统 MQTT 首次连上后调用一次（60 秒内连不上则不再等），业务 MQTT 经 `cmd/config` 更新后再调用一次；业务层据此（重新）建立自己的连接。两条连接错开握手：在单核的 C3 上同时做两次 TLS 握手会连续数秒占满 CPU，触发任务看门狗告警。业务连接的重连间隔也应与系统连接（5 秒）错开。

业务连接的客户端 ID 不能用设备 UUID（系统 MQTT 在用，同一 Broker 上会互相挤下线），也不能用系统 MQTT 的账户和主题。基座的状态文档、命令、OTA 与业务层无关，target 是 `<芯片>/<业务工程名>`，OTA 只在同一 target 之间进行。

构建、签名、刷写和正式打包用本仓的 `tools/`，以 `ESP_BASE_FIRMWARE_ROOT` 指向业务固件根；正式打包另设 `ESP_BASE_APPLICATION_ID` 为 Fast Deploy 身份里的应用 ID。

## 公网下载

固件经公网（跨境链路常有丢包）用 HTTPS 下载，`sdkconfig.defaults` 为此调了三处 TCP 默认值：接收窗口 23,040 B（默认 5,760 B，链路好时下载被它限速）、开启 SACK、乱序队列放到上限。下载任务（`components/base_ota/ota_worker.c`）连续 20 秒收不到数据就放弃当前连接，退避后从已写入的扇区续传；`esp_https_ota` 自己把读超时当作继续等待，不会结束一条不再送数据的连接。

## 签名与回滚

应用用 RSA-3072（Secure Boot V2 方案）签名，`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` 在更新时验签，信任根是当前运行固件签名块里的公钥。bootloader 不验签，也不烧写 eFuse，所以有线刷写始终可用。

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 打开。新固件首次启动后有 300 秒连上 Broker，连上才调用 `esp_ota_mark_app_valid_cancel_rollback()`；超时、崩溃或看门狗复位都会回到旧固件。同一任务累计回滚 3 次记为失败。

验证自动回滚时，用 `CONFIG_ESP_BASE_TEST_NO_BROKER=y` 构建一个只联网、从不连接 Broker 的测试固件（额外的 sdkconfig 片段经 `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;<片段>"` 传入），用同一把键签名后经 OTA 下发。正式打包入口拒绝打开了这个开关的镜像，测试固件验收后从登记库删除。

## 测试

```bash
bash tests/run_host_tests.sh
```

覆盖协议编解码、命令去重，以及 OTA 状态机的断电与停滞续传、重试退避、拒绝、回滚计数、取消和换任务，并用[合同样例](../docs/design/device-protocol-samples.json)核对与 Server 一致。host 测试不写设备；实板验证见仓库 README 的三层验证。
