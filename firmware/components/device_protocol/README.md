# 设备协议组件

本组件在唯一控制 owner 上复用 USB／MQTT／FRP 的严格请求解码、持久设备 UUID、本 boot 身份、期限、32 槽写请求守卫和统一结果序列化。完整 wire 合同见[设备控制协议](../../../docs/design/device-protocol.md)。

USB／Bridge 保留 status、restart、config.set、firmware.status、ota.start／ota.result 和 business.status／pause／resume。FRP 仅开放显式管理端点；固件上传认证后借给 OTA worker，不把镜像加入 JSON 命令缓冲。MQTT 复用同一命令及独立原生事件 HMAC。配置写入仍要求物理 USB，管理／OTA／重启／配置试运行按现有 owner 互斥。

原生业务同步借用认证事件内存，处理完成后才推进 event sequence；没有新增业务 FIFO、heap 副本或业务线程。当前 boot 最近完成事件的原始字节 SHA-256、结果和高水位通过非 retained reported 输出。暂停后的业务拒绝保持计数，属于已处理的业务失败，避免将 Broker PUBACK 当作设备业务成功。

OTA 请求在登记 V4 意图并独立读回后转交 worker；原请求存活至 worker 发布完成，查询和第二请求不能替换它。FRP arm 同样在读回后创建，5 秒内接入的流绑定 operation/device/boot/长度/摘要；传输结束仍须验签、切槽和新 boot 本地确认。取消后的流不获选槽。结果不确定时保留原 operation，不能自动另起擦写。

每轮物理 VFS 最多读取 256 B 并让出调度，半帧 2 秒排空；C3 无缓冲 VFS 保留 FIFO 背压，ESP32 UART 实板回归待接入。网络只在 Wi-Fi／可信时间与配置满足时启动；OTA 本地确认不依赖 Broker／FRPS 在线。

host 的 ASan／UBSan 与真实 loopback HTTP/HMAC 回归不替代实板、正式 FRPS、Flash 时延或容量峰值。输入和结果见[原生软件检查点](../../../docs/operations/native_software_checkpoint.md)。
