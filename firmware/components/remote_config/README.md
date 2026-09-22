# remote_config

拥有完整配置、单调 revision 和 NVS 提交边界。候选验证由单一控制任务调度，本组件只在连接证明成立后提交。

## 架构拓扑

```mermaid
flowchart LR
    app["apps/esp_base：启动读取"] --> store["esp_base_remote_config：条件提交 / 读回"]
    owner["device_protocol：候选与连接证明"] -->|"expected_revision + 完整配置"| store
    store --> codec["config_codec：类型校验 / 规范字节"]
    store --> nvs["base_store/base_config/committed：单个 NVS blob"]
    nvs -->|"revision 与配置一起恢复"| app
    identity["nvs/base_identity：独立持久身份"]
```

P2 支持 Wi-Fi station 配置；MQTT、FRP 和业务配置尚未接入，JSON 中必须为 null。Wi-Fi 未配置也是 null；配置时 SSID 为 1–32 个 UTF-8 字节，不含控制字符；密码为 8–63 个可打印 ASCII 字符，或 64 位十六进制 PSK。接入至少 WPA2-Personal，不静默尝试开放网络。

存储使用既有 `base_store` 分区，唯一 key 为 `base_config/committed`。112 字节规范编码包含版本、revision 和完整 Wi-Fi 配置，不保存 C 结构填充、指针或 SDK 私有布局。写入前重新读取 revision，拒绝冲突和溢出；完整 blob 写入、`nvs_commit` 与读回一致后才成功。旧迁移代码读取的 `nvs/base_config/generation` 在已核对实板上不存在，不保留兼容读取或双写。

配置候选只在 RAM；没有连接证明不调用写接口。写入开始后的错误为 `storage_uncertain`，不能假设原值未变，也不自动重试；控制任务重新读取真实持久状态并停止后续写命令，待重启重新核验。读取失败或格式损坏停止初始化，不自动擦除。身份 namespace 和分区布局保持不变。

NVS blob 的底层原子更新与掉电恢复使用[官方 NVS 实现](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_flash.html)。host 故障注入验证调用层的错误语义，不能替代真实断电测试。
