# C3 产品 HTTPS 安装检查点

2026-10-02，C3 原始 Base 的公开产品安装、代表事件、健康确认、重启恢复与卸载实板切片通过。此记录与空产品 USB／MQTT 及原生取消实验分别保存。

## 输入与边界

执行源码为 `b8d695838328b3664f983baeb7dafc992d5f3982`，与公开 `2c4941ea90594cab2cf36467a33135846e4bf3c9` 的运行输入逐字节相同。原始启动入口、正式 C3 分区、SDK 与四份依赖锁保持；未注入固件调用方。官方 SDK 的两个自定义证书 bundle 配置只在私有构建中加入既有实验 CA，原源码、默认 CA bundle 和生产信任均未修改。该实验镜像的证书配置与此前默认构建不同，不能作为默认生产镜像验收。

C3 RSA v2 签名 app 为 1183744／1245184 B，SHA-256 `e5c2651f97997d372213ac13e524502c4293bd6a97aa150c129b32a63cbc7ba7`，官方验签通过。4 MiB 写入与完整读回逐字节一致。HTTPS 与 MQTT 服务仅监听本轮宿主物理 IPv4，复用既有有效实验服务证书；CA、主机身份、日期和可信时间条件均保持。HTTPS 返回固定 200、精确 Content-Length，无分段传输或重定向。

产品包 SHA-256 `3d71095bdc1af6e202ac01b58f19c2b484f2e0b126dec85fbce9f42a99a2dd99`，整包 10240 B、Wasm 469 B。它由固定 wasi-sdk 33 编译公开 Container `2b93b97` 的 counter 源码，以既有 RSA-3072 测试密钥签名；验签公钥与固件产品授权 DER 精确一致。产品为 `counter/v0-1-0`、guest ABI 2、数据 schema 1、一页 65536 B 内存、4096 B Wasm 栈、空能力集合，不能据此验收日志、定时器或其他宿主导入。

## 实际链路

维护者授权丢弃 ESP 数据后，从空白 NVS 与包槽首启，原公开 USB 工具完成 Wi-Fi／MQTT 配置版本 0→1→2。严格 TLS、当前 boot 的非 retained reported、认证查询与既有 HMAC／QoS／配置写门负例通过。

公开 `product.install` 只发送一次，初次按原操作 ID 查询为 `unknown/product_operation_unresolved`，保留该未决结果。设备实际取得 TLS 1.2 的完整 10240 B 包并验签，候选 `active_product` 的摘要、版本、ABI／schema、trial 与操作 ID 均按真实状态核对。公开事件发布器随后只发布一帧与本设备、boot、包和代表事件摘要绑定的 HMAC 事件；设备 reported 返回 sequence 1、`succeeded` 与 guest result 19。

原固件的连续 30000 ms Wi-Fi／可信时间／MQTT 与代表事件健康门完成后，原安装 ID 查询持久成功，确认绑定的 ECS2 序号由 1 进入 6，活动实例退出 trial；写入到首次成功查询为该轮 41.078 秒，包含下载、装载、事件观察与轮询，不是性能上界。

只发布一次 MQTT restart，在原 5 秒期限内取得 running。USB 与新 reported 核对同设备的新 boot、配置版本 2、MQTT 恢复及相同序号 6 确认绑定；公开 counter 已由真实 boot 路径重新装载。新 boot 的第二帧代表事件再次得到 sequence 1、`succeeded` 与 guest result 19。

公开 `product.uninstall` 只发送一次，操作序号 2 的原 ID 持久查询成功，ECS2 6→7。十字段状态独立回读为空绑定、无活动实例、包 ABI／schema 为 null，高水位 2、下一操作序号 3。此前的安装／重启写命令没有自动重发。

## 证据与清理

54 份成功证据索引 SHA-256 为 `9683f46a5101084cdff35e36d4db0eedf72668378144202da5a6584cda74b627`，包括实际脚本、原公开工具、逐条回执、USB 写命令元数据、HTTPS／MQTT 原始记录、两次事件发布器结果、签名构建、Flash 读回及清理证明。构建与宿主夹具另有 27 份独立索引，SHA-256 `cdd6cdfa529e5da3e56c811e5a5385ba4324cb292b140f79fa3ce93b3eeb59dc`。输入未传完、签名参数缺失及生成脚本语法检查的前序失败保留，没有执行不完整脚本写板。

证据在 ESP Tool 私有忽略目录 `provisioning/receipts/private/c3-validation-20261002/` 保存，复制前后摘要一致。实验结束整片擦除测试数据，只恢复原三份代码制品并逐字节读回；Wi-Fi 关闭、串口释放、HTTPS／Broker／控制客户端停止，无 eFuse 写入。此实验没有永久完成正式布局迁入。

ESP32 未连接；正式产品下载入口、升级、完整宿主能力／异常管理、FRP／OTA 联网、五能力峰值、掉电与 72 小时仍待验收。P6-04／P6-09 保持进行中，不能将这一页 counter 切片计作完整产品或生产发布。
