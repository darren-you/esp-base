# ESP32 移栈后 FRP 认证阶段超时的只读诊断备忘

2026-09-27。只分析[同锁容量收据](esp32-frps-stack-workspace-qemu-capacity-checkpoint.md)中已归档的仓外动态 TLS 输入，不启动新的 QEMU，不修改产品代码或配置。输入为 Base 6ef7a028c2a5e16ea62fab6a0942bf7500479cd9、FRP 9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3 及该收据固定的 MQTT／OTA／Container／WAMR／IDF／lwIP 锁。签名 app SHA-256 为 eccf2c2921745559dff478aee6b691398ed09ae887353919debf4f0506b5ec2d；原始 UART 为 mac-work-1:/private/tmp/esp-frp-workspace-stack-session-20260927/attempt-dynamic-worker-4096/qemu-uart.log，SHA-256 为 9cdb4e134a7e1721434b7d3cde56f58d8b425074afdfebe8bcaab47a659588a3。FRPS 原始日志 SHA-256 为 7a2f4b332cfa54b705bbc8a8e8d97444a06d59e7a03e198d2b7801d1803d2b30。

## 原始日志确定的路径

- OpenETH DHCP 得到地址后，仓外探针设置测试时钟 1790503200，Base 正式 SNTP 门仍为 false。客户端先在 QEMU 时间 37,353 ms 进入 TLS_HANDSHAKING(2)，再于 38,793 ms 进入 AUTHENTICATING(3)，当时 MALLOC_CAP_8BIT free／largest／minimum 为 5,908／5,376／5,908 B。
- 第一次在 43,823 ms 进入 BACKOFF(7)：failure_phase=AUTHENTICATING(3)、EFRP_TIMEOUT=-9、tls_error=0、verify=0、ready=0、pongs=0，堆最低 944 B。后续三次从 AUTHENTICATING 到同样超时的间隔约 5,070／5,000／5,010 ms；未进入 REGISTERING 或 READY。观测结束时第五次尝试尚在 TLS_HANDSHAKING。
- 第一次退避时失败分配回调累计 173 次，最近一笔 1,532 B／caps 6144；第四次退避后累计 691 次。探针回调只保存累计计数与最近一笔，不保存各次申请的调用栈、时间和返回地址。固定 IDF 的 caps 6144 是 MALLOC_CAP_INTERNAL｜MALLOC_CAP_DEFAULT。第三次尝试临近超时时，UART 另有 opencores.emac 的 no mem for receive buffer；固定 SDK OpenETH 接收任务在该错误路径申请 ETH_MAX_PACKET_SIZE＝1,522 B 后失败。它证明第三次尝试存在接收缓冲容量故障，不能反推出第一次超时的直接原因，亦不能将 691 次计数归为某一调用点或泄漏。

当前精确 FRP 锁的 client.c 只有在 efrp_tls_step 成功、随后 efrp_session_create 成功时才把阶段从 TLS_HANDSHAKING 切到 AUTHENTICATING。tls_mbedtls.c 在握手完成时用 mbedtls_ssl_get_verify_result 检查证书并要求零标志；配置为 MBEDTLS_SSL_VERIFY_REQUIRED。故本次至少证明客户端完成严格 TLS 握手、验证仓外 CA／IP SAN，并在本地成功创建 session。session.c 的 create 路径初始化 Hello/Login 输出并在本地 Yamux 控制流排入 SYN，但这些是本地内存状态，**不能据此说 Hello 或 Login 已写入 socket**。

服务端 fixture 把官方 FRPS v0.71.0 日志级别设为 error，归档 frps.log 只有 QEMU_FRPS_READY／STOPPED。官方服务端 service.go 中，TLS 或 Yamux 接收故障、首帧处理、RegisterControl、Login 拒绝与 LoginResp 写失败的日志分别处于 trace／info／warn，均被该级别过滤。原始输入也没有抓包或客户端应用字节计数。因此，现有证据中最后可确认的网络收发是**客户端完成 TLS 握手并收到有效服务端证书**；没有证据确定 FRPS 是否收到 Hello／Login、是否拒绝，或是否发出 LoginResp。未进入 REGISTERING 只说明客户端没有完成 LoginResp 处理。

## 超时来源的边界

第一次 AUTHENTICATING 阶段样本至退避样本相隔 5,030 ms。精确 FRP 锁里，TLS 写入期限 EFRP_TLS_IO_MS 和 Yamux 无进展期限 EFRP_YAMUX_IO_TIMEOUT_MS 均为 5,000 ms；TLS 待写、Yamux 输出、部分输入均可返回 EFRP_TIMEOUT。Hello/Login 握手期限 EFRP_HANDSHAKE_TIMEOUT_MS 为 10,000 ms，初次认证阶段的控制注册／Pong 期限尚未启用。现有镜像没有编入 FRP 已存在的 EFRP_LAB_TIMEOUT_TRACE，状态快照也没有 TLS pending bytes、Yamux 待输出／部分输入或超时来源。5 秒时间差支持短 I/O 期限触发，但**不能在 TLS 写入与 Yamux 收发间定责**；tls_error=0 对这两类路径都可能出现。

固定锁中的 session.c 依次调用 TLS 进度、Yamux tick、握手进度、控制和 transport 收发；在认证阶段任何一步返回非 OK 都会经 client.c drain 记为 failure_phase=3。已归档的官方 FRPS host 集成测试验证了不同资源条件下的会话与 provider 接线，不能替代本次 ESP32 QEMU 网络／内存条件。只读证据不足以认定确定性 FRP 源码错误，因此本轮不修改 FRP 或 SDK，不运行新的 QEMU，也不把认证超时归因于服务端拒绝、单次内存泄漏或正式 Base owner。仓外额外 4 KiB 探针任务、测试时钟和 Base 正式 SNTP=false 的边界保持不变。

如以后需要继续定责，应作为**新的、单独签名输入**记录本端现有超时 trace、TLS pending 和应用字节实际交付计数，并提升仓外 FRPS 日志级别以观察 Login 入口；新观测不能回填本次镜像的容量读数。
