# ESP Base

ESP32-C3 与 ESP32 的业务基座固件：设备身份、Wi-Fi、TLS MQTT 管理和公网 OTA，业务以 WASM 应用的形式由固件里的宿主加载运行。只依赖官方 ESP-IDF 发行版、组件注册表和原样引用的官方 WAMR 发行版，不维护任何上游派生。

固件分两层：**系统层**负责联网、系统 MQTT、命令、远程配置、OTA 和设备 Profile；**业务层**只实现自己的功能，通过业务运行时（`base_runtime`）这一个接口取得设备身份、业务参数和业务 MQTT 配置，创建任务、打开自己的连接。业务看不到系统的 Wi-Fi、系统 MQTT 账户和 NVS，系统的升级与管理也不经过业务代码。这是代码上的解耦，不是内存隔离：两层在同一个镜像里（C3 这类芯片没有条件做真正的隔离）。

- **管理**：一份 retained 状态文档（即设备 Profile：名称、所属 Group、芯片、固件、MAC、IPv4／IPv6、Wi-Fi、MQTT 地址与运行状况），命令走 `cmd/<名称>` 并按 `request_id` 应答去重，遗嘱消息表示离线。这条系统 MQTT 连接只由出厂资料配置；远程配置改 Wi-Fi、业务 MQTT（留给业务层另开连接）、设备名称和所属 Group。
- **OTA**：Server 经 MQTT 发布期望固件，设备用官方 `esp_https_ota` 通过 HTTPS 拉取，断线或断电后按 Range 续传；镜像用 RSA-3072 签名，新固件连上 Broker 才确认有效，否则由 bootloader 回滚。
- **事件驱动**：一个控制任务阻塞在事件队列上，空闲时不轮询；Wi-Fi 使用默认省电模式，只在 OTA 下载期间关闭。
- **安全模式**：业务任务的优先级低于系统任务；同一个固件连续 3 次异常重启就不再拉起业务，系统照常在线，可以远程升级修复。

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

出厂资料可以带设备名称 `name`（1–32 字节，不含控制字符，首尾不是空格；不给时设备上报「型号-MAC 后 3 字节」，之后也能在网页里改）；所属 Group 只在网页里设置。还可以带业务固件需要的内容：业务 MQTT 的 `business_mqtt_uri`（`mqtts://`）、`business_mqtt_username`、`business_mqtt_password` 三项同时给出；`business` 对象写业务参数，键是最多 15 个字符的小写字母、数字、下划线，值是字符串，业务固件里用 `base_runtime_param` 读取。

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

## 业务应用（WASM）

业务用 Rust 写成 `no_std` 的应用，编译成 WASM，由基座固件里的 WASM 宿主加载运行：应用是事件驱动的，只能经宿主接口使用定时器、业务 MQTT 等设备能力，出故障时只停应用，系统层照常在线。SDK 和样例见 [sdk/rust](sdk/rust/README.md)，宿主见[固件说明](firmware/README.md#wasm-宿主)。目前应用还是随固件内嵌的样例，独立部署尚未实现。

## 业务固件

业务以独立的 ESP-IDF 工程在本仓之上开发：引用本仓的组件、配置和分区表，在 `app_main` 里调用 `base_system_run` 并交出业务描述（名称和两个回调），业务组件只依赖 `base_runtime`。接口和做法见[固件说明](firmware/README.md#业务固件)；本仓的工具以 `ESP_BASE_FIRMWARE_ROOT` 指向业务固件根即可构建、签名、刷写和正式打包。

## 正式包

正式固件包由 Fast Deploy 的 package Job `esp-base-esp32-firmware`、`esp-base-esp32c3-firmware` 构建（声明见 `package/<芯片>/deploy.yaml`），在工作区根执行 `bash harness/scripts/workspace/fast-deploy.sh trigger --job esp-base-esp32c3-firmware --wait`。两种芯片共用构建号 scope `firmware / esp-base`。Job 在 mac-ci-1 写出本次身份文件，执行 `tools/package_entry.py`：构建，用 Profile 快照中 `esp-base/firmware/shared.prod.env` 的 `ESP_BASE_SIGNING_KEY_PEM`（两种芯片共用这一把 RSA-3072 键）签名并验签，再封装成含 `manifest.json` 与 `build.json` 的 ZIP；Fast Deploy 再回读包内身份并留存制品。Fast Deploy 不导出 ESP-IDF，入口会自己 source 官方 `export.sh`，再用 IDF 的 Python 重新执行；IDF 默认在构建宿主的 `~/.espressif/frameworks/esp-idf-v6.1`，可用 `ESP_BASE_IDF_PATH` 指定别处。上面第 4 步的 `--package` 刷写的就是这个 ZIP。

## 文档

- [设备—云协议](docs/design/device-protocol.md)：主题、状态文档、命令和 OTA 合同；[样例](docs/design/device-protocol-samples.json)同时用于设备和 Server 的解码测试。
- [固件说明](firmware/README.md)：目录、分区、配置项和测试。
- [应用包](docs/design/app-package.md)：业务应用的包格式、清单和签名。
- [业务应用 SDK](sdk/rust/README.md)：用 Rust 写业务应用。

## 开发

```bash
bash firmware/tests/run_host_tests.sh   # host 测试：协议编解码与 OTA 状态机，需先构建过一次
```

验证分三层：host 测试、两块板冒烟（刷入后联网、收发命令、完成一次 OTA）、24 小时长稳。host 测试通过不代表实板可用。长稳用 esp-tool 的 [`server/scripts/ota_soak.py`](https://github.com/darren-you/esp-tool/blob/1.0.0/server/scripts/ota_soak.py)，在 Server 所在主机上运行：两块板在两个已登记的版本之间来回升级，记录每轮结果、非预期重启和最小空闲堆。

依赖只有官方 ESP-IDF、组件注册表中的 [espressif/mqtt](https://components.espressif.com/components/espressif/mqtt)、[espressif/cjson](https://components.espressif.com/components/espressif/cjson)，以及按精确提交原样引用的 [WAMR](https://github.com/wasm-micro-runtime/wasm-micro-runtime) 2.4.6，版本由 `firmware/dependencies.lock.<target>` 锁定。首次构建时组件管理器要从 GitHub 取回 WAMR 的完整仓库（约 250 MB，之后走本机缓存）。

问题反馈和计划登记在 [darren-space Issues](https://github.com/darren-you/darren-space/issues)。

## 许可证

[Apache-2.0](LICENSE)。
