# ESP32 旧 ESP-AT 配置只读检查点

2026-09-27，以 P5-05 两份私有 4 MiB ESP32 完整 Flash 备份及 P1-04 已保存的 AT 查询收据作**仓外只读**核对。两份备份逐字节相同；使用本仓固定 ESP-IDF 的官方 NVS parser 检查旧 `nvs@0x12000/0xe000`，只有前两页非 `0xff`，页状态、页 CRC 与已写入记录的 CRC 均有效。未打开设备串口，也未写设备、备份或生产密钥；本页不记录任何凭据、MAC、UUID 或私有镜像摘要。

## 可证明的配置与身份事实

- `nvs.net80211` 中 `sta.ssid`、`sta.pswd`、`sta.pmk`、`sta.bssid`、`sta.apsw`、`sta.apinfo`、`ap.ssid`、`ap.passwd` 的**完整数据区**均为 `0xff`，没有可迁入 Base v3 的旧 Wi-Fi SSID 或密码。P1-04 保存的 `AT+CWJAP?` 响应为 `No AP`，与此相符；不能据此推断历史上从未连接过网络。
- 旧 NVS 的 `sta.mac` 与 `phy/cal_mac` 可在私有输入内同 P1 eFuse MAC 交叉核对。物理 MAC 可保持，但旧 ESP-AT 没有 `base_identity/device_uuid`；Base 设备 UUID 不能从 MAC 推导，也不建立旧身份到新 UUID 的映射。新 Base 读不到 UUID 时会在自己的 NVS 中生成 UUIDv4，这是源码路径，尚未在该 ESP32 实板验收。
- 旧 `at_customize@0x20000/0xe0000` 只有前两页非 `0xff`：二级分区表和首个 `ble_data` 页。八个二级分区条目与公开 ESP-AT `v1.1.0.0` 的 `at_customize.csv` 布局相同；这不能证明现物固件的精确源码提交，也不能把 `ble_data` 或未识别 AT/SDK 标量解释成 Base 活动配置。

## 归档与新配置的边界

现有 16 KiB 旧区归档器对 `nvs` 和 `at_customize` 的非空原始页做仓外 0600 归档，已在两份完整备份上验证**各完整旧分区**可逐字节及 SHA-256 重建。原始旧分区可归档恢复，与旧字段迁入 Base v3 活动配置是两件事；完整 Flash 恢复仍依赖两份原始完整备份。旧 Wi-Fi 凭据为空，本轮不生成 v3 配置候选或身份映射。

Base 源码在 `base_store/base_config/committed` 缺失时加载空配置，随后仅能通过获授权的物理 `config.set` 提供新 Wi-Fi、MQTT、FRP 配置。这个结论只覆盖源码与离线备份：尚未证明 ESP32 新布局首次启动、空配置实板行为、双签名槽、旧启动链切换或恢复。它们仍须在独立实板维护窗口按设备事实和完整恢复基线验收。

公开对照：[固定 ESP-IDF NVS parser](https://github.com/espressif/esp-idf/blob/578cf89c343e388db43ba1f4ddcd602fedcb763c/components/nvs_flash/nvs_partition_tool/nvs_parser.py)、[ESP-AT 二级分区表](https://github.com/espressif/esp-at/blob/v1.1.0.0/at_customize.csv)。
