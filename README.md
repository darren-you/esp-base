# ESP Base

ESP32-C3 与 ESP32 的业务基座固件：设备身份、Wi-Fi、TLS MQTT 管理和公网 OTA。只依赖官方 ESP-IDF 发行版和组件注册表，不维护任何上游派生。

- **管理**：一份 retained 状态文档，命令走 `cmd/<名称>` 并按 `request_id` 应答去重，遗嘱消息表示离线。
- **OTA**：Server 经 MQTT 发布期望固件，设备用官方 `esp_https_ota` 通过 HTTPS 拉取，断线或断电后按 Range 续传；镜像用 RSA-3072 签名，新固件连上 Broker 才确认有效，否则由 bootloader 回滚。
- **事件驱动**：一个控制任务阻塞在事件队列上，空闲时不轮询；Wi-Fi 使用默认省电模式，只在 OTA 下载期间关闭。

支持的硬件：ESP32-C3、ESP32-D0WD-V3（芯片版本 v3.0 及以上），4 MiB Flash。不启用硬件 Secure Boot、Flash 加密，不写 eFuse。

## 快速开始

需要 macOS 或 Linux、Python 3，以及官方 [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/get-started/index.html)：

```bash
git clone --branch v6.1 --recursive https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32c3,esp32
source ./esp-idf/export.sh
```

以下命令在本仓根目录执行，`<target>` 是 `esp32c3` 或 `esp32`。

**1. 构建**。产物是未签名的 `firmware/build/<target>/esp_base.bin`：

```bash
python3 tools/esp_base.py build <target>
```

**2. 签名**。设备只接受与当前运行固件同一把键签名的更新，所以同一批设备始终用同一把键。键放在仓库之外；第一次可以用 `espsecure generate-signing-key --version 2 --scheme rsa3072 <键文件>` 生成：

```bash
python3 tools/esp_base.py sign <target> --key-file /absolute/path/signing-key.pem
```

**3. 准备出厂资料**。在仓库之外写一个 JSON 文件，权限设为 `0600`：

```json
{
  "device_id": "<小写 UUID>",
  "wifi_ssid": "<2.4 GHz Wi-Fi 名称>",
  "wifi_password": "<Wi-Fi 密码>",
  "mqtt_uri": "mqtts://<Broker 域名>:8883",
  "mqtt_username": "<设备账户>",
  "mqtt_password": "<设备密码>"
}
```

**4. 刷写**。首次刷写先整片擦除，再写入 bootloader、分区表、固件和出厂资料：

```bash
python3 tools/esp_base.py flash <target> --port /dev/cu.usbmodemXXXX --erase --provision /absolute/path/device.json
```

以后有线更新固件用 `--app-only`，不动出厂资料。USB 转串口在 460800 下不稳时加 `--baud 230400`。刷正式构建的 ZIP 时加 `--package <ZIP>`：文件和偏移都取自包内 `manifest.json`，写入前逐个核对摘要。刷完后串口日志会打印一行 `ESP_BASE_BOOT`，包含设备 UUID、版本、运行槽和固件 SHA-256。

**5. 查看状态和下发命令**（需要 `python3 -m pip install 'paho-mqtt>=2.1'` 和一个有权限的 Broker 账户）：

```bash
python3 tools/esp_base.py control status --device <UUID> --uri mqtts://<Broker 域名>:8883 \
  --username <账户> --password-file /absolute/path/password
python3 tools/esp_base.py control cmd restart --device <UUID> --uri ... --username ... --password-file ...
```

## 正式包

正式固件包由 Fast Deploy 的 package Job `esp-base-esp32-firmware`、`esp-base-esp32c3-firmware` 构建（声明见 `package/<芯片>/deploy.yaml`），在工作区根执行 `bash harness/scripts/workspace/fast-deploy.sh trigger --job esp-base-esp32c3-firmware --wait`。两种芯片共用构建号 scope `firmware / esp-base`。Job 在 mac-ci-1 写出本次身份文件，执行 `tools/package_entry.py`：用 Profile 快照中 `esp-base/firmware/shared.prod.env` 的对应签名键构建、签名、验签，封装成含 `manifest.json` 与 `build.json` 的 ZIP；Fast Deploy 再回读包内身份并留存制品。执行环境须已导出官方 ESP-IDF。上面第 4 步的 `--package` 刷写的就是这个 ZIP。

## 文档

- [设备—云协议](docs/design/device-protocol.md)：主题、状态文档、命令和 OTA 合同；[样例](docs/design/device-protocol-samples.json)同时用于设备和 Server 的解码测试。
- [固件说明](firmware/README.md)：目录、分区、配置项和测试。

## 开发

```bash
bash firmware/tests/run_host_tests.sh   # host 测试：协议编解码与 OTA 状态机，需先构建过一次
```

验证分三层：host 测试、两块板冒烟（刷入后联网、收发命令、完成一次 OTA）、24 小时长稳。host 测试通过不代表实板可用。

依赖只有官方 ESP-IDF 和组件注册表中的 [espressif/mqtt](https://components.espressif.com/components/espressif/mqtt)、[espressif/cjson](https://components.espressif.com/components/espressif/cjson)，版本由 `firmware/dependencies.lock.<target>` 锁定。

问题反馈和计划登记在 [darren-space Issues](https://github.com/darren-you/darren-space/issues)。

## 许可证

[Apache-2.0](LICENSE)。
