# ESP32 FRP 首个控制流 TLS 发送停滞的仓外检查点

2026-09-27。本检查点接续[FRPS trace 与超时字段收据](esp32-frps-auth-output-qemu-trace-checkpoint.md)，用两个**彼此独立的新签名镜像**观察首个 Yamux 控制流 SYN 的 TLS 写入。基础源码仍是 Base `6ef7a028c2a5e16ea62fab6a0942bf7500479cd9`、FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`、MQTT `c0677e5e779c3e51e814f2920420be7ec54f1d88`、OTA `d98361f348e19e965efd7462277dde0ae13056fa`、Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 和 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。原始六组件重算审计日志 SHA-256 为 `64275d6e6c989121ed79ea2cd9292ccc5d3771da158908584372505e0b6a91f3`；本轮除注明的仓外 FRP 两个源文件外，其余受管源码与该已审计副本逐文件一致（复制时省去嵌套 `.git` 与 `.component_hash`）。两个镜像的 `sdkconfig` 均为 `1a4a148b1e23bd6c197e7dc4abda2a44ea3590a1e9244922b6d61d8964baea4d`：仓外 SDK 原生动态 TLS buffer 开启，内容上限 16,384／4,096 B 不变，`EFRP_LAB_TIMEOUT_TRACE` 关闭。正式 FRP、Base、SDK 源码和配置没有改动。

每次都使用同一个官方 FRPS v0.71.0 仓外 trace fixture（二进制 SHA-256 `5393dfec485ba5092bab9bceb6cd769a123f0dd6b233b5dcddf7a037d3706663`）、本机回环 `127.0.0.1:29372`、OpenETH、4,096 B **额外**探针任务、6,144 B FRP worker、同一临时测试 CA（SHA-256 `84eaa94649239e6d406f906dbc7a2ac4a3f47f2783ec4b7c8e3adae8be26818d`）和 IP SAN。每个新 app 都用仓外测试键签名、经官方 `espsecure verify-signature --version 1` 验签，再按该 app 完整摘要重建 ECS2 sequence 6 confirmed 绑定与全新合成 4 MiB Flash。ABI 2 包 SHA-256 为 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`。探针只在仓外设置 `time(NULL)=1790503200`；FRP 原生时间判断仍执行，**Base 正式 SNTP `time_ready=false`，正式 owner 无法启动**。没有写实体设备、正式分区或正式凭据。两个 app 均为 `0x10fff4` B，`0x120000` B 槽余 `0x1000c` B。

## 前置打印输入：首笔写入在 socket 前返回内存错误

第一份只在仓外 FRP 的 `tls_mbedtls.c`、`session.c` 中采集 TLS step、`send` 回调、`tls_staged`／Yamux consume 与同点 free／largest；`session_stage` 和 `tls_step_enter` 在首次 `mbedtls_ssl_write()` 前直接打印。补丁 SHA-256 为 `752522d658b40e00a9627ebc68eb580df5dd8fe634a80e66405bf08ce7a5e4de`，修改后两个源文件分别为 `a72f94c3d2715efce8d64227916525b8f3b483eb20fecfc95ade494c8905d30a`、`75baff35ef2f7957bf1ba8120a17383b20e24128ca2557ff5a7a1b24b2573461`。签名 app SHA-256 `501bdb10337c50522cde284f0eb955b0db52b8955a8ae9c12be7821a5a5b66ea`，官方验签有效。

首个 Yamux SYN 为 12 B，已在本地 `tls_staged=12`。第一次 TLS 写入前 free／largest 为 **5,876／3,840 B**；`mbedtls_ssl_write()` 原始返回 `-141`，即固定 PSA 头文件的 `PSA_ERROR_INSUFFICIENT_MEMORY`，FRP 映射 `EFRP_NO_MEMORY=-20`。`send_calls=0`，所以这一输入在 socket 回调前失败；严格 TLS 清理快照仍为 `verify=0`，FRPS 只见 TLS 后 `AcceptStream() EOF`。`-141` 没有附带内部调用栈，不能精确指认为某一笔 TLS 输出缓冲申请。前置打印已改变本输入的连续堆余量与停止点：它**不能**解释先前未插桩镜像的 5 秒超时，也不能和下份镜像合并容量数字。

原始目录 `mac-work-1:/private/tmp/esp-frp-stack-tls-send-probe-20260927/`；准备脚本 SHA-256 `eeb110d247ff363fa673b5c0c31a94977cd6cd426fbb60ba77086dfcbffad863`，runner `ac6c745726d56dd5f8f0419c2c4e0267c1abd9aa2f7fb3c5d2053e59d0331dbf`，构建日志 `6a6b7dfd8318dbc9fba35e834fde761e7a163a6a5cf6d21279aa7271942f4559`。`seeded-flash.bin` SHA-256 `347c543427c9d40ea566e0948ad4b436460c0c1feda2c0563e8a105a9cff9eb0`，运行后 `qemu-flash.bin` `7a19bdb70dc8e8c813a0fa053bcca11380306c1ea0c3b9dda92f73d6c45fb8d8`，原始 UART `0aeb8b4ed3f1700cf37d0555b03949cfc7400a8e310d3d890c0320d29163bea6`，FRPS 日志 `0d4a7accdf3b9ed494fdca325e997ef8c93f616192f6534478f5faea9ed6b2a9`，运行命令 `71a9aaa0be14e9adc93fadbffea5c7b99e3ec130a3f26ff4507e467f383a0d47`。

## 延迟打印输入：首个应用记录被 socket 持续拒绝

第二份重新从已审计的原始 FRP 锁源码复制，**不在正常发送路径打印**：仅在结构内累加 TLS 原始返回值、首次／最后写入前堆、send 次数／结果／errno／字节数和 session stage／consume 次数，失败后打印一次。补丁 SHA-256 `273a0e617389f865d41cc388e4ab87293c40a2f666fd9ec4e90a72b0db53f0d4`；两个修改后源文件分别为 `a3f76cfadcd94ff01876c48fee5580a855bf2a5ac3c2890d5f7aa1afedb59ee5`、`06e0f717cba5aec7716934c46b0558b15ecd7a4505a68e176bd7fe3c3e172aed`。新签名 app SHA-256 `4561c24bcdeb6d9014c1996fbae822c9560467db5652e4e5f0b9fbb7d6c9a3d9`，官方验签有效；ECS2 和 Flash 全新生成。

该输入恢复原先的认证 5 秒超时：前四次都进入 `AUTHENTICATING(3)` 并以 `EFRP_TIMEOUT=-9` 退避，结束时第五次仍在 `TLS_HANDSHAKING(2)`。第一至三次由 Yamux 输出期限先触发；失败前仍有 `tls_staged=12`、`tls_pending=12`、`want=2`（WANT_WRITE），`stage_count=1`、`consume_count=0`。第四次由 TLS 写入期限先触发，清理前直接记录：

| 同一第四次尝试的证据 | 原始值 |
| --- | ---: |
| 首次 `mbedtls_ssl_write()` 前 free／largest | 5,908／4,864 B |
| 最后一次写入前 free／largest；整次启动堆低水 | 1,348／496 B；944 B |
| TLS 原始返回、待写、状态 | `-26752`＝`MBEDTLS_ERR_SSL_WANT_WRITE`；12 B；`EFRP_TLS_OPEN` |
| TLS 写入／socket 发送调用 | 178／178 次 |
| `send()` 成功／WOULD_BLOCK | 0／178 次 |
| 最后 `send()` 请求／已接受／errno | 41／0 B／11（固定 lwIP `EAGAIN`） |
| session 队列进入／Yamux consume | 1／0 次，首段 12 B |

这份输入的下层 `send()` 回调在第四次认证窗口内 **178 次均返回 WOULD_BLOCK，未接受应用记录字节**；最后一次 errno 为 11（EAGAIN），前 177 次的逐次 errno 未保存，不能全称 EAGAIN。TLS 的 `WANT_WRITE` 因此有明确 socket 回调证据，而非凭阶段名推断。原始 FRP `connect.c` 对 EAGAIN／EWOULDBLOCK／EINTR 返回 `EFRP_WOULD_BLOCK`，TLS `send_bytes()` 将其变为 `MBEDTLS_ERR_SSL_WANT_WRITE`。这一调用链与 12 B 待写、Yamux consume=0、官方 FRPS 在五次 TLS 识别后都于 `AcceptStream() EOF` 一致。第五次 EOF 发生于 runner 收尾，不能记为第五次认证超时。服务端没有接受首个控制 stream，没有 `client login info`、拒绝或 LoginResp；本轮不能称已发送或收到 Hello／Login。严格 TLS `verify=0` 仍成立。单次日志中另有 OpenETH `no mem for receive buffer`，可证内存压力同时存在，但无法仅由 EAGAIN 定位 lwIP 内部哪笔资源或队列，也不能证明 FRP 源码缺陷、单次泄漏或正式 owner 的停止点。

原始目录 `mac-work-1:/private/tmp/esp-frp-stack-tls-send-lean-20260927/`；准备脚本 SHA-256 `e4264f2f87c1dc511b6f41712a545e2190331efd637b19f70554dd4dbc5543a6`，runner `d20c5e1896969bbea3c53b2e43bd40e89f2d5d1660b6cd6a548073a3bb94503f`，构建日志 `f868b8f01cfa66dfc4b48815d55fabba8e7ed85bb537139e69da159f9fbcd404`。`seeded-flash.bin` SHA-256 `02d1a80128aebc04428dbb9228deeb8c0f9d516b5a69705d48c6cf208e6fade0`，运行后 `qemu-flash.bin` `5457401bd279ddd76ce62b47ed69c2a5fb4ccfa443852ae03e5e4419f235b2a1`，原始 UART `057d854ece7054c1e37b8b312f07a58d0b9e948fe665207925af7db4d3725fe3`，FRPS 日志 `9ccab1a327c177926782c8ee892eeeb52e8e9c26f81e5fca4a8f5c161aafe9a2`，运行命令 `ba8a3fae4453e92133930a1015593935fc0343d443413520b16bebd8b870a6d3`，验签输出 `3d4c27b901e82c0e6d5a10e896068f7d5937b51aa76ba48e80cbdbdbbfa3bff6`。结束后 QEMU 与本机 FRPS 均已停止。

## 使用边界与下一锁成本

两份仓外插桩均改变 FRP 组件二进制和瞬时堆，只有第二份可与旧超时输入作**机制上的交叉佐证**，不能把它的 `free／largest／min` 合入旧容量对照，更不能据此裁决 48 KiB／24 KiB 门。4 KiB 测试任务并非 Base 正式 FRP owner；正式 SNTP 仍 false。保持正式源码、协议限额、任务栈和分区不变；P6-03 的正式登录／会话、并发、实板与容量验收继续开放。

当前 canonical Base 已前进到 `2604dd9796a9855e66978561f528292e6964bc1b`，不能把本旧 Base 输入的签名 app 或 Flash 当作新锁复测。可复用的是仓外准备脚本、FRPS fixture、runner 与阶段采样方法；需要从新 Base 独立导出、重新核对六组件／IDF／lwIP 锁、把探针适配进新源码、全量签名构建与验签、按**新 app 摘要**重建 ECS2／Flash 后再跑。计算量约为一次 1,161 步 ESP32 全量构建与一轮最多 100 秒的 QEMU 运行，另需源码适配及来源审计；不可省略这两项。运行时须独占 `mac-work-1` 的 QEMU OpenETH 实例与回环 FRPS `127.0.0.1:29372`，检查无其他 QEMU／FRPS 进程，并使用独立仓外目录、测试签名键和新 Flash 副本；不与 guard 的 QEMU 并行。
