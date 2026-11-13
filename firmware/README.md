# ESP Base 固件

构建、签名和刷写步骤见[仓库 README](../README.md)，协议见[设备—云协议](../docs/design/device-protocol.md)。工程结构遵循 [Embedded Firmware 工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded-firmware/embedded-firmware-golden-path.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `apps/esp_base/main` | 主程序：控制任务、MQTT 客户端、状态文档、命令执行 |
| `components/base_protocol` | 主题、命令解码、应答与去重、原生业务；纯逻辑，不依赖 ESP-IDF |
| `components/base_ota` | OTA 状态机（纯逻辑）和基于 `esp_https_ota` 的下载任务 |
| `components/base_store` | NVS 中的设备 UUID、配置和 OTA 记录 |
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

设备 UUID 和 Wi-Fi／MQTT 配置在 NVS 的 `base` 命名空间，由 `tools/esp_base.py flash --provision` 生成 NVS 镜像写入；固件出错时不擦除 NVS。经 `cmd/config` 下发的新配置先试用，连上 Broker 才保存，120 秒内连不上就恢复旧配置。

## 签名与回滚

应用用 RSA-3072（Secure Boot V2 方案）签名，`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` 在更新时验签，信任根是当前运行固件签名块里的公钥。bootloader 不验签，也不烧写 eFuse，所以有线刷写始终可用。

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 打开。新固件首次启动后有 300 秒连上 Broker，连上才调用 `esp_ota_mark_app_valid_cancel_rollback()`；超时、崩溃或看门狗复位都会回到旧固件。同一任务累计回滚 3 次记为失败。

## 测试

```bash
bash tests/run_host_tests.sh
```

覆盖协议编解码、命令去重、原生业务，以及 OTA 状态机的续传、重试退避、拒绝、回滚计数、取消和换任务，并用[合同样例](../docs/design/device-protocol-samples.json)核对与 Server 一致。host 测试不写设备；实板验证见仓库 README 的三层验证。
