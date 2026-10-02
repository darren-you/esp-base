# C3 FRP 与 MQTT 管理实板检查点

2026-10-02，C3 的原始 Base 启动入口、正式分区和现有网络 owner，在隔离官方 FRPS 与 TLS Broker 下完成认证状态、错误密钥拒绝及一次 FRP 重启。运行源码与 `b8d695838328b3664f983baeb7dafc992d5f3982` 逐字节一致，没有注入固件调用方。该轮没有产品绑定；它补齐 C3 管理链路实证，不能代替产品、OTA 与五能力容量验收。

软件合同见[认证状态](frp_authenticated_status_checkpoint.md)、[认证重启](frp_restart_checkpoint.md)及[设备协议](../design/device-protocol.md#frp-base-软件接线边界)。

## 精确输入

| 对象 | 本轮输入 |
| --- | --- |
| SDK / lwIP | `578cf89c343e388db43ba1f4ddcd602fedcb763c` / `2758df4cd3666b3b2a5b53830148379326425c0d` |
| FRP / MQTT | `8f056273b3b93ea3273b4637038ddd0c6aea82a8` / `50c9c45f0fe95d4e99ab39584ff04d45d432efbc` |
| Container / WAMR | `2b93b979b8b0760dcb96b28ac5d13fc52ae547bf` / `74fd95ccbdc417c3816e04f3308eea8a5473ed34` |
| 私有签名 app | 版本 `0.2.0`，RSA v2，`1183744/1245184` B，SHA-256 `1fd9e100205d9995568a5c1806ad19103657782cc982ce693c6c4d802832e7ff` |
| 官方 FRPS | `0.71.0`；从 `frp-service@cb370d93286109931dafe6e0d9a740e832db9834` 已冻结的官方 darwin-arm64 归档提取 |
| 官方归档 / FRPS 字节 | SHA-256 `45be02b186860d375ed49a8941ae9569628a54bf14e67fc36b29c98c99dabcc6` / `71a4896060db4a9290bd830f48561334a3660545a0907c29dfade42f91f57037` |
| Tool 控制库 | `6bf7dabc91a5044bc61be7c06587772e7262f324` 的原始 Server 归档，SHA-256 `83fc5e5c9da539bc0aabbc06555c53fdd12e0f0b94c6bc9e9b77aa956c9fa607` |
| 实板观察器 | SHA-256 `a8bee826fe1b36003cebda08e565ac1b0a7c008fadb58af7d6e346abe974d92d` |

该 app 复用 [WRITE 实验](c3_joint_ota_write_message_counter_checkpoint.md)的来源 A：仅私有实验 CA 与 timer 授权／一个定时器的官方配置不同。本轮没有装载 guest，也没有发送安装、升级或 OTA。源文件、依赖锁、正式分区和生产信任未改；测试签名键没有写入 eFuse。

官方归档原 catalog 登记的是 FRPC。该轮只在私有目录提取同归档中的官方 FRPS，核对摘要和实际版本；没有把实验服务登记为受管运行面，没有改变生产 FRPS 或 catalog。

## 实际调用与结果

1. 重新核对唯一 C3、芯片与安全状态；完整擦除、写入私有签名镜像并回读。使用维护者提供的 Wi-Fi，通过物理 USB 写完整配置，revision 从 0 经 1 到 2；可信时间、严格 TLS MQTT、当前 boot 的非 retained reported、认证查询和配置写门通过。
2. 仅由 USB 写一次完整 FRP 配置，revision 为 3。设备从 connecting 到 ready；官方 FRPS 强制 TLS、Token 心跳／工作连接鉴权及 TCP mux，管理代理只转发设备既有 loopback 端点。
3. 私有 CLI 仅装配 Tool 原有 `Controllers.ReadStatus`／`Restart`，使用 `go build -mod=readonly` 构建。通过 HTTPS 网关、官方 FRPS 和设备 listener 读取实际状态；Tool 验证设备 SDK PSA HMAC 签发的原始响应，核对同设备／boot、revision 3 及 MQTT／FRP ready。
4. 改变实验管理 key 的一个字节后，设备返回空正文 401 且没有响应认证 tag，Tool 返回失败；恢复正确实验 key 的新只读查询成功。
5. 调用既有 `Controllers.Restart` 一次，取得设备签名 202，再通过同一 FRP 路径确认同设备新 boot。网关日志只有一条 restart POST，没有写重放或其它重启通道。USB 独立读回新 boot，随后 MQTT 与 FRP 均恢复 ready，revision 3 和设备 UUID 保持，空产品账本高水位为 0、下一序号为 1。

网关共记录 7 次请求，包括一次错误 key 的 401 和一次签名 restart 202。202 本身不是成功判据；成功依据是 Tool 经 FRP 读到同设备新 boot，及后续独立 USB／MQTT／FRP 状态核对。HTTPS 网关保留原始正文及认证 tag，只做转发；CLI 不是生产 Server 的 Auth／SQLite 操作入口。

## 内存与清理

| 状态 | free heap（B） | 历史最低 heap（B） |
| --- | ---: | ---: |
| 首次 FRP 认证状态 | 144736 | 125680 |
| 新 boot 的 FRP 认证状态 | 142756 | 132724 |

这些读数来自空产品组合。两个 boot 的历史最低值分别记录，不能把它们当作同一连续压力区间；没有证明最大合法 FRP 记录、guest 运行或 OTA 下载期间达到 48 KiB 门。

观察器正常退出。已停止该轮独占的 FRPS、HTTPS 网关及 MQTT Broker／控制循环；丢弃实验数据，仅恢复原 bootloader、partition table 与 factory app，并逐字节核对三份代码制品。旧 NVS、实验 NVS、配置和身份均未恢复；原代码启动后 Wi-Fi down 回执与串口释放已确认。没有 eFuse 写入。

## 证据与未完成范围

ESP Tool 私有 receipts 的 `c3-validation-20261002/frp-mqtt-management-pass` 保存 72 份原始文件及独立索引；索引 SHA-256 为 `1bd99e8328834bffb55bc389eee12535835411b81a87ec7ef001f9a00126d598`。源码归档、官方二进制、构建配置、请求／响应、串口与 Flash 回读、清理状态均逐项核对后迁入持久私有目录。凭据、设备标识和原始网络地址不进入 Git。

C3 的空产品 FRP/MQTT 管理切片通过。产品和 FRP/MQTT 的同机运行、OTA 并发与 scratch 仲裁、最大合法记录及五能力堆／连续块／栈门、ESP32、两板隔离、生产 Auth／账本／网关路径、掉电与 72 小时仍待验收；P4-05、P6-03 和 P8-03 保持进行中。
