# 乐鑫官方能力与 ESP Base 固件选型

2026-10-09按当前源码、双目标分区、唯一组件清单和依赖锁修订。[乐鑫仓库清单](espressif-repository-catalog.md)保留2026-09-22的公开资料快照，目录存在不代表当前生产依赖。

Base采用锁定ESP-IDF v6.1／lwIP和独立MQTT、FRP、OTA组件；C3／ESP32均为4MiB、双`0x1e0000`app。消息计数直接编译入固件，由唯一控制owner处理认证事件。当前实现与实际验收分别见[固件说明](../../firmware/README.md)、[原生软件检查点](../operations/native_software_checkpoint.md)和[唯一执行计划](../operations/ota-allocation-diagnostic-checkpoint.md)。

| 能力 | 当前消费者与边界 |
| --- | --- |
| ESP-IDF | Wi-Fi／netif／event、NVS、SNTP、TLS、分区／app_update、WDT和复位事实；精确源码由[sdk-lock.json](../../sdk-lock.json)固定 |
| Component Manager | [唯一组件清单](../../firmware/components/device_protocol/idf_component.yml)声明MQTT／FRP／OTA完整提交与cJSON，分别生成[C3](../../firmware/dependencies.lock)与[ESP32](../../firmware/dependencies.lock.esp32)锁 |
| ESP-MQTT | 官方派生公开组件拥有协议／outbox／QoS与有界运行层；Base拥有设备Topic、认证命令、原生事件与结果 |
| HTTP／app_update | 公开OTA组件复用官方机制，验证完整签名镜像、摘要、芯片／项目／长度与A/B写槽；Base拥有写前V4收据、互斥、本地确认和原ID结果 |
| FRP | 独立公开组件拥有TLS／TCP／Yamux与流预算，Base装配设备端点、HMAC、短Flashowner及有界固件上传；FRP属于已定产品链路，不是乐鑫官方协议组件 |
| 主机工具 | 固定SDK的官方分区／NVS／签名工具用于离线输入核验；Mac App通过Rust核心／Swift薄绑定消费实际USB的ROM／Flash能力 |

设备自身FRP传输完整固件字节；本机USB的URL型OTA由设备HTTPS拉取，两条路径共用升级事务。退役动态业务运行体系的可选装配、源码仓与专属实验入口已清除，没有第二条引擎路线。

当前未取得双目标真实公网／App／USB、同存峰值、Flash最坏延迟、断电、每板百次和连续72小时资格，正式交付仍待。软件通过不等于设备签名启动或生产入口通过；每轮物理写入须核对唯一目标、安全状态、新鲜双份完整恢复基线与设备租约。实施依据为[嵌入式工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded-firmware/embedded-firmware-golden-path.md)。
