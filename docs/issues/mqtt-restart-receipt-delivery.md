# MQTT 重启回执交付

2026-10-02，正式 Base 的 C3 隔离 Broker 实验未在原有 5 秒期限内收到 MQTT 重启回执。该轮已清理恢复，写命令只发送一次，整体记为失败。

## 实际观察

实验使用 `94323b19ca72a5c33fac10a2efc7d8e77f3f4049` 的原始启动入口与正式分区，C3 RSA v2 测试签名 app 为 1183744 B，SHA-256 `260735e57baa0aa7ba001cec8c9f347db6f3638431be7dee7d031719bb2f81cb`。未注入固件测试调用方。宿主复用既有有效测试证书，使用独立实验账号、严格 TLS、精确设备 Topic ACL 和 paho-mqtt 2.1.0；生产 Broker、凭据与服务没有修改。

真实 C3 完成 Wi-Fi 配置版本 0→1、MQTT 配置版本 1→2，取得当前 boot 的可信时间、实际 MQTT ready 与非 retained reported。认证状态查询返回成功，当轮固件摘要匹配；错误 HMAC 与 QoS 0 命令各观察两秒没有结果，随后同请求 ID 的有效帧都成功。MQTT `config.set` 返回 `failed/physical_usb_required`，USB 独立回读版本仍为 2。

随后一次 MQTT `restart` 没有交付 running 结果，观察者在原始 5 秒期限内超时；Broker 记录了设备 offline。该轮没有完成新 boot 验收，不将断连当作重启成功。原始日志与代码共 18 份文件冻结为私有失败证据索引，SHA-256 `b4f30be3515dde43e2889434b9f2e63e8e223721bc07d84f9350fd8dccd8cdc0`。原代码三份制品逐字节恢复，实验 NVS 丢弃，Wi-Fi 关闭、串口释放、临时 Broker 与控制客户端停止，没有 eFuse 写入。

## 执行顺序修正

原处理器将 running 回执放入 MQTT 发送队列后，固定等待 100 ms 就调用重启。入队不是 Broker 收到，也不是调用方取得结果；该路径没有观察真实 PUBACK。

MQTT owner 现在只跟踪一份重启回执的精确 QoS 1 消息 ID。只有匹配 ID 的实际 PUBACK 才记录 Broker 交付证据；其他结果的 ACK 不计，会话断开、网络失效、重配或失败均撤销该证据。回执入队失败时不安排重启，并缓存 resource_failure 结果。

处理器安排重启后立即返回 MESSAGE 回调，使唯一控制任务继续消费 MQTT 事件。控制循环在最短 100 ms 后观察匹配 PUBACK；尚无交付证据时继续等待至 2000 ms 观察期限。回执丢失仍是未确认，不自动重发写命令；PUBACK 只证明 Broker 交付 running 回执，实际重启成功仍须由同设备的新 boot 与配置回读证明。控制循环不是硬实时调度，不宣称这两个观察阈值构成绝对时延上界。

等待期间沿用原全设备写门：新的 MQTT、USB 或 FRP 写命令返回忙，同 ID 跨通道返回原 running 结果，不创建第二次重启。USB 仍按原同步回执路径执行，FRP 仍按原签名 HTTP 回执与有界排空合同执行。

## 验证边界

两目标完整 host 回归已通过。MQTT owner 用例验证错误消息 ID、精确 PUBACK、另一个结果不覆盖被跟踪 ID、重复追踪拒绝、无效长度、入队失败及会话失效撤销。协议用例验证处理器返回后才重启、最短观察阈值、无 ACK 的观察期限、重复请求不重执行、跨通道写门和入队失败不重启；既有 USB 与 FRP 用例继续通过。

初轮使用了不属于真实组件合同的事件枚举名，编译失败日志保留，已改用固定组件的 `EMQTT_EVENT_PUBACK`。随后旧跨通道用例仍假设 MQTT 处理器立即重启，其失败也保留；当前用例先验证等待与去重，再观察实际 ACK。没有放宽生产协议或改 SDK。

## 固定 SDK 与 C3 复验

执行源码 `b8d695838328b3664f983baeb7dafc992d5f3982` 的双目标固定 SDK 完整构建、官方签名校验与容量检查通过。C3 RSA v2 app 为 1183744／1245184 B，SHA-256 `bb204cbe50d10f63236c5efcbe0737d7926a69176d4fbc2f5fc8ac6739f06609`；ESP32 ECDSA v1 app 为 1114100／1179648 B，SHA-256 `066c5a3bc99d1da0f6513ddc61390acf90eb1a1aea5d999d376df9872320f59e`。SDK、两目标配置、四份依赖锁和原分区未变。

原始启动入口的正式 C3 签名镜像在空白数据实板复验通过，没有注入固件测试调用方。Wi-Fi／MQTT 配置版本为 0→1→2；严格 TLS、可信时间、当前 boot 的非 retained reported、认证状态查询、错误 HMAC／QoS 0 拒绝及远程配置写门均通过。只发布一次 MQTT restart，在原 5 秒回执期限内取得对应 running；随后 USB 与新的非 retained reported 独立核对同设备的新 boot、配置版本 2、联网恢复与当轮状态字段。首次 MQTT ready 的 uptime 为 35684 ms，重启后为 4484 ms；这些是该轮采样值，不是性能保证。

这轮 49 份文件冻结为私有成功证据索引，SHA-256 `10f3de06acef686bdd0101e044249e978d14d725df530b297fe4975a1683fd84`。它与前述失败索引分别保存，包含实际测试输入、原始回执、串口／Broker／发布与接收记录、双目标回归、签名构建及恢复证明。4 MiB 写入逐字节读回一致；结束后丢弃实验数据、逐字节恢复原三份代码制品、关闭 Wi-Fi、释放串口并停止实验 Broker／控制客户端，没有 eFuse 写入。

该切片只验证当轮Base USB／MQTT控制与一次重启，ESP32未连接。当前原生固件、设备FRP OTA、双板峰值／断电／长稳和正式交付仍按[唯一执行计划](../operations/ota-allocation-diagnostic-checkpoint.md)取得独立资格，已退役的运行路线不再执行。
