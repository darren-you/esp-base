# C3 复用产品包联合 OTA 检查点

2026-10-02，原始 Base 的公开 `REUSE` 联合 OTA 实板切片通过：C3 从已确认产品与固件 A 升级至固件 C，实际代表事件和连续健康窗口完成后，原 OTA 操作持久成功；再次重启后仍查询到同一成功结果，产品重装载与公开卸载通过。

## 精确输入

运行源码为 `b8d695838328b3664f983baeb7dafc992d5f3982`，与公开 `8bb80dbfc51bfa51e0fc54e9780b67f7c3c5631d` 的运行输入逐字节相同。原启动入口、正式 C3 分区、四份依赖锁与固定 SDK 保持，未注入固件调用方。SDK 为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`。

来源 A 复用[公开安装实验](c3_product_https_install_checkpoint.md)的私有 CA 构建，版本为 `0.2.0`，RSA v2 签名 app 为 1183744／1245184 B，SHA-256 `e5c2651f97997d372213ac13e524502c4293bd6a97aa150c129b32a63cbc7ba7`。目标 C 使用同一源码、策略、签名测试键与实验 CA，仅增加官方 SDK 的版本配置，实际镜像描述版本为 `0.2.0-c3-lab-c`；签名 app 同为 1183744 B，SHA-256 `7f8263d712993e18a189e0c59207151ef47151de3e76e008b51de1285a9d8e33`，官方验签通过。实验 CA 和版本设置均在私有构建，生产信任未修改。

产品仍为 `counter/v0-1-0`：整包 10240 B、Wasm 469 B、guest ABI 2、数据 schema 1、一页内存、空能力集合，包 SHA-256 `3d71095bdc1af6e202ac01b58f19c2b484f2e0b126dec85fbce9f42a99a2dd99`。本轮复用这个包，没有以新包代替 REUSE。

## 实际链路与结果

维护者授权丢弃设备数据后，整片擦除、写入 A 并完整读回。公开工具提交 Wi-Fi／MQTT 配置版本 0→1→2；严格 TLS、可信时间、当前 boot 的非 retained reported、认证查询和 HMAC／QoS／远程配置写门负例通过。

公开 `product.install` 只发送一次。代表事件由公开发布器只发送一帧，设备返回 sequence 1、成功及 guest result 19；原连续 30000 ms 健康门完成后，原安装 ID 持久成功，ECS2 1→6，产品退出 trial。

公开 `ota.start` 只发送一次，以同一已确认包执行 `reuse`，严格 HTTPS 下载签名固件 C 至 `ota_1`。当前设备进入不同的新 boot，配置版本仍为 2。原 OTA ID 首先返回 running；新 boot 的代表事件再次返回 sequence 1、成功及 guest result 19。沿原 90 秒只读观察窗口查询同一 OTA ID，实际取得 27 次 running、2 次 `unknown/ota_result_uncertain`，随后为 succeeded；两次 unknown 保留为当时事实，未重发升级命令。确认后的产品绑定为 C 和原包，ECS2 6→10，活动实例已退出 trial。

只发布一次 MQTT restart，原期限内取得 running。同一设备进入第三个不同 boot，配置版本 2 和确认产品恢复，ECS2 保持 10。原 OTA ID 再次查询为 succeeded，固件摘要、镜像长度、目标槽、reuse 与包摘要均精确匹配；第三次代表事件同样成功，guest result 19。

公开 `product.uninstall` 只发送一次，操作序号 2 的原 ID 持久成功，ECS2 10→11。十字段状态回读为空绑定、无活动产品、包 ABI／schema 为 null，高水位 2、下一序号 3。完整 ECS2 序列为 1→6→10→10→11。

HTTPS 共记录四次 TLS 1.2 完整 GET：宿主预检包和固件各一次，设备实际下载各一次；均为固定 200、精确 Content-Length、完整摘要匹配，无分段传输。最后进入 ROM，从 `0x20000` 读取完整 A、从 `0x150000` 读取完整 C，实际字节分别与原 A 和目标 C 逐字节一致。

## 观察器修正与证据

首轮观察器在 28 次 running 后，把合同允许的 `unknown/ota_result_uncertain` 提前当作结束条件，未观察最终结果；60 份失败证据索引 SHA-256 为 `dedf101949d7dca890b6c287afce05120bf8f5132620f51d2a506a41d6c88998`。这不能证明固件失败。复验只修正调用方的只读观察：沿原 90 秒窗口接受原 ID 的 running 与有完整绑定结果的 unknown，只有精确 succeeded 才通过；固件、原命令期限和成功判据未改变。

75 份成功证据索引 SHA-256 为 `b88a8f180a96529edfcfa9c5ab11ed784ff296bff100512a6ea1da0bfcb1a460`，包括实际调用脚本、公开工具、完整回执、三次事件发布结果、HTTPS／MQTT 原始记录、官方目标签名构建、A／C Flash 读回与清理证明。证据位于 ESP Tool 私有忽略目录 `provisioning/receipts/private/c3-validation-20261002/`，复制前后字节和摘要一致。

实验结束整片擦除测试数据，只恢复原三份代码制品并逐字节读回。Wi-Fi 关闭、串口释放、HTTPS／Broker／控制客户端停止，无 eFuse 写入；未永久完成新分区迁入。

本轮只证明 C3、实验信任与空能力 counter 的 REUSE 路径。WRITE、新产品升级、完整宿主能力、正式生产下载入口、FRP、五能力峰值、掉电、ESP32 与 72 小时仍待验收，不能据此关闭 P6／P7／P8 总门。
