# 源码来源

维护者拥有的第一方代码从私有归档中的 `esp_service@22bc4f767a53edec8941be9b35af2c2f09f6acb1` 选择迁入，作者 darren-you，新仓按 Apache-2.0 分发。归档 SHA-256：`05eeb54825d07c68ff4734f076f9f3a3bfe39f21dc1de38110fad20f869f85fb`；该归档不作为构建依赖或公开下载。

| 原路径 | 本地路径 | 保留与修改 |
| --- | --- | --- |
| firmware/components/device_identity | 同路径 | 保留 UUID 校验、NVS namespace/key 和硬件读取 |
| firmware/components/device_protocol | 同路径 | 保留串口心跳基线，后续替换为统一控制协议 |
| firmware/components/remote_config | 同路径 | 保留 generation 读取，待实现配置事务 |
| firmware/components/ota_runtime | 同路径 | 保留槽读取；不在启动时直接 mark-valid |
| firmware/components/safety_runtime | 同路径 | 保留复位原因与 WDT 初始化 |
| firmware/apps/esp_base/main | 同路径 | 移除 GPL POC 装配和 NVS 自动全擦 |
| firmware/partitions、sdkconfig.defaults | 同路径 | 保留 C3 4 MiB 与 A/B 分区 |

未复制 `xfrpc`、`frpc_runtime`、POC connectivity、json shim、专用测试、旧 Git 历史、凭据、固件二进制与实板身份。SDK 来自 [Espressif ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1)，以官方许可作为构建依赖；不是本仓自研源码。官方 MQTT 与独立 FRP 尚未加入此迁移基线的构建。
