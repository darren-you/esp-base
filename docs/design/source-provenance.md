# 源码来源

维护者拥有的第一方代码从私有归档中的 `esp_service@22bc4f767a53edec8941be9b35af2c2f09f6acb1` 选择迁入，作者 darren-you，新仓按 Apache-2.0 分发。归档 SHA-256：`05eeb54825d07c68ff4734f076f9f3a3bfe39f21dc1de38110fad20f869f85fb`；该归档不作为构建依赖或公开下载。

| 原路径 | 本地路径 | 保留与修改 |
| --- | --- | --- |
| firmware/components/device_identity | 同路径 | 保留 UUID 校验、NVS namespace/key 和硬件读取 |
| firmware/components/device_protocol | 同路径 | 保留设备事实上报，接入有界 JSON 命令、UUID 启动身份与设备结果 |
| firmware/components/remote_config | 同路径 | 替换只读 generation 为类型化配置、独立分区单 blob 提交与读回 |
| firmware/components/ota_runtime | 通用机制迁至公开 esp-ota；Base 自有策略与收据在 firmware/components/ota_operation | 本地自检后显式确认，失败时拒绝 pending 槽；不保留通用下载、槽或摘要实现 |
| firmware/components/safety_runtime | 同路径 | 保留复位原因与 WDT 初始化 |
| firmware/apps/esp_base/main | 同路径 | 移除 GPL POC 装配和 NVS 自动全擦 |
| firmware/partitions、sdkconfig.defaults | 同路径 | 保留 C3 4 MiB 与 A/B 分区，新增 ESP32 独立 4 MiB 分区与目标配置 |

未复制 `xfrpc`、`frpc_runtime`、POC connectivity、json shim、专用测试、旧 Git 历史、凭据、固件二进制与实板身份。当前设备 SDK 的精确源码为公开 [受控 ESP-IDF 来源](https://github.com/darren-you/reference-esp-idf/tree/fb53f8a76df5ea913715658f5ac602e91a094e72) `fb53f8a76df5ea913715658f5ac602e91a094e72`；原业务修正基线为迁出前 `esp-space/esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c`，受控来源只追加 Actions 退出和命中的递归来源绑定，基于官方 [Espressif ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1) `fff9895c82d744c7237be8847347bdd1b07c6643`，修正 `app_update` 擦除失败后 OTA handle 遗留，以及 `esp_http_client_init` 在 transport 加入列表前低内存失败时的句柄遗留；lwIP 子模块固定公开 [darren-you/esp-lwip](https://github.com/darren-you/esp-lwip) `2758df4cd3666b3b2a5b53830148379326425c0d`，修正零窗口 ACK 循环。这两个提交由仓根 `sdk-lock.json` 声明并由构建检查；SDK 仍为外部 Apache-2.0 等原许可的源码依赖，不复制为本仓自研。普通 Base 消费公开 [darren-you/esp-frp](https://github.com/darren-you/esp-frp)，精确提交以[唯一组件清单](../../firmware/components/device_protocol/idf_component.yml)和双目标锁为准；Base 保留配置、owner、Flash scratch 接线与设备端点门，不复制协议栈或旧 GPL POC。

JSON 解析通过 Component Manager 依赖 [espressif/cjson 1.7.19~2](https://components.espressif.com/components/espressif/cjson/versions/1.7.19~2/readme)，组件摘要与 IDF 版本分别固定在 C3 的 `firmware/dependencies.lock` 和 ESP32 的 `firmware/dependencies.lock.esp32`。来源为 Dave Gamble 与 cJSON contributors，MIT 许可保留在官方依赖中；没有复制为本仓自研源码。命令边界校验、行读取与裁决由本仓实现。

Wi-Fi 生命周期、候选控制与配置 codec 为本仓新增实现，调用 ESP-IDF v6.1 的 esp_wifi、esp_netif、esp_event、NVS 和 PSA SHA-256；未复制旧 FRP 调试连接器或重写 SDK 驱动/密码原语。

MQTT 从公开 [darren-you/esp-mqtt](https://github.com/darren-you/esp-mqtt) 获取，组件名仍为 `mqtt`，精确完整提交由唯一组件清单及两目标依赖锁固定。该仓以官方 [ESP-MQTT v1.1.0](https://github.com/espressif/esp-mqtt/tree/1a1e5788a5cf57a0f44a3c6c061407f6c9be1026) 为基线并保留 Apache-2.0 许可；维护源码修正 clean session 断线后旧 SUB/UNSUB outbox 重放、运行层复用消息槽遗留旧 Topic／payload 尾部，让入站消息体按实际存活期分配，并在经典 ESP32 单核／8BIT IRAM 条件下放置运行实例与消息体。通用 `emqtt_` 运行层归该仓；Base 只保留持久 UUID、设备 Topic/LWT 与设备命令归属，不持有 `mqtt_runtime` 或官方 Registry 的第二份 MQTT 依赖。真实 Broker 的源仓回归与 Base 宿主回归分别留证，当前精确组合的设备 Broker/TLS 尚未实板验收。

OTA 通用机制由公开 [darren-you/esp-ota](https://github.com/darren-you/esp-ota) 提供，精确提交由唯一组件清单与双锁声明；Component Manager 在两目标依赖锁中锁定其 `components/esp_ota`。该仓[来源记录](https://github.com/darren-you/esp-ota/blob/master/docs/design/source-provenance.md)绑定迁自 Base 的精确源码与许可；Base 仅维护产品约束、operation 收据、自检及调度。退役旧备用固件前的 HTTPS URL authority 预检、OTA 逐扇区擦写及 app／otadata 读写的 I/O 回调由 OTA 库统一提供。

消息计数业务在本仓 `firmware/components/native_business` 实现，直接链接入 app，由现有控制 owner 同步执行认证二进制事件。2026-10-09 已清除退役动态运行体系的源码仓引用、专属工具与历史实验正文，依赖链只保留上述官方 SDK 和真实公开组件。旧持久记录只由一次性离线迁入工具识别，用于保留身份、配置与恢复边界，不作为运行依赖。
