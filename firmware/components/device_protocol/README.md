# 设备协议组件

本组件在唯一控制 owner 上复用 USB／MQTT 的严格请求解码、持久设备 UUID、本 boot 身份、期限、32 槽写请求守卫和统一结果序列化。完整 wire 合同见[设备控制协议](../../../docs/design/device-protocol.md)。设备 FRP 任务、监听器、端点和专属观测已从源码移除。

USB 保留 status、restart、config.set、firmware.status、ota.start／ota.result 和 business.status／pause／resume。MQTT 复用控制命令，并使用独立原生事件和 OTA 块 HMAC 通道。配置写入要求物理 USB 的 schema 4；管理／OTA／重启／配置试运行按现有 owner 互斥。

原生业务同步借用认证事件内存，处理完成后才推进 event sequence，没有新增业务 FIFO、heap 副本或业务线程。reported 输出当前 boot 的业务序号、摘要和实际结果；Broker PUBACK 不等于设备执行成功。

OTA 请求在登记 V4 意图并独立读回后转交 worker；原请求存活至 worker 发布完成，查询和第二请求不能替换它。MQTT 使用固定单块 mailbox，数据块不占普通写守卫；实际 Flash 写入成功且短 claim 释放后才推进确认。完整 END、长度、摘要、签名和目标校验之后，仍须通过取消／超时边界才能选槽；新 boot 本地确认与持久终态沿用已有机制。结果不确定保留原 operation。

每轮物理 VFS 最多读取 256 B 并让出调度，半帧 2 秒排空；C3 保留 USB 背压，ESP32 使用 UART ISR。网络只在 Wi-Fi／可信时间与配置满足时启动，OTA 本地确认不依赖 Broker 在线。host ASan／UBSan、HMAC 和双线程测试不替代正式 Broker、实板 Flash 时延或资源峰值；完整交付资格仍由 #35 验收。
