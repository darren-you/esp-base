# ESP32 当前五仓锁的签名 guest／FRP 会话容量检查点

2026-09-27，P6-03 仓外窄测。固定 Base `087f9baaa4414a75106103b46249c80b61189564`，FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`、MQTT `c0677e5e779c3e51e814f2920420be7ec54f1d88`、OTA `d98361f348e19e965efd7462277dde0ae13056fa`、Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`，以及 Base `sdk-lock.json` 的 IDF／lwIP。Base 的 `git archive` gzip SHA-256 为 `f98de6c519bf471dad12138ceb5af63cca49ddb2b762b804bf628ffa9edb7395`；Component Manager 构建日志逐项回读上述版本，ESP32 生成锁 SHA-256 为 `5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`。

仅在 `mac-work-1:/private/tmp/esp-frp-current-session-20260927/` 的无 Git 归档加入 3／4／8 KiB 测试任务、QEMU OpenETH 和本机 FRPS 探针，使用 Base 已绑定并启动恢复的正式 `efrp_idf_flash_store_callbacks`，调用公开 `efrp_create → efrp_start → efrp_get_status → efrp_destroy`。合成 4 MiB Flash 使用上一轮仓外测试签名 ABI 2 counter 包、当前新签名 app 摘要重建的 ECS2 sequence 6 confirmed 绑定、独立 FRP scratch 和六页 NVS；候选分区 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`。固定 SDK 以 ECDSA v1 测试键签名并经官方 `espsecure verify-signature --version 1` 分别验证各次 app；签名尺寸均为 `0x10fff4`，双 `0x120000` app 槽各余 `0x1000c`。没有实体设备或正式分区写入。

宿主用官方 `github.com/fatedier/frp v0.71.0` 的 `server.NewService` 在 `127.0.0.1:29372` 运行强制 TLS／测试 Token；临时 CA 和含 `10.0.2.2` IP SAN 的证书只在仓外，客户端配置 `MBEDTLS_SSL_VERIFY_REQUIRED`。QEMU guest 经 OpenETH DHCP 得到 `10.0.2.15`，签名 guest 为 `RUNNING`，Base 到达 `READY container=running`。Base 正式 `esp_base_time_ready()` 始终为 **false**，首次使用正式时间回调的尝试在客户端创建前返回 `EFRP_TIME_UNTRUSTED`。后续两次容量诊断只让**仓外测试客户端**使用 `time(NULL)` 在 2024-01-01 至 2030-01-01 内的时间回调；日志明确为 `qemu_rtc_trusted=1 base_sntp_ready=0`。这不是正式 Base 的 SNTP 门，更不授权正式 FRP owner 启动。证书校验配置虽严格，因容量失败发生在握手前，**没有实际证书验签结果**；`verify=4294967295` 是未验证哨兵值。

## 当前锁真实停止点

下表为 4,096 B 仓外任务栈、同一签名 guest 与官方本机 FRPS。数字是 `MALLOC_CAP_8BIT` 的 free／largest／启动以来 minimum，单位 B；失败回调另记录申请尺寸与能力。阶段间有异步任务，`phase_changed` 是清理期间采样，不等同失败调用瞬时堆快照。

| 阶段 | free／largest／minimum | 结果 |
| --- | ---: | --- |
| guest `RUNNING`、Base `READY`、探针开始 | 47,620／26,624／43,032 | 启动低水已小于 48 KiB 门 49,152 B |
| OpenETH DHCP 后 | 33,496／26,624／28,748 | `got_ip=1`，正式 SNTP 门仍 false |
| `efrp_create` 前 | 33,372／26,624／28,648 | 仓外时间回调 true |
| `efrp_create` 后 | 22,732／20,480／22,500 | API 成功，client 为 `STOPPED` |
| `efrp_start` 已排队 | 22,152／20,480／22,132 | `CONNECTING=1`、attempts=1 |
| 清理期间的 `FAILED=8` | 20,772／18,432／**512** | `failure_phase=CONNECTING(1)`、`EFRP_NO_MEMORY=-20`；失败回调一次，申请 **4,429 B**，caps **2052** |

固定 `sdkconfig` 为 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384`、`OUT_CONTENT_LEN=4096`，`CONFIG_MBEDTLS_DYNAMIC_BUFFER` 关闭。固定 SDK 的 `mbedtls_ssl_setup` 先申请 **16,717 B** 入站 record 缓冲，再申请 **4,429 B** 出站缓冲；两个数分别是配置内容长度加上此 SDK 的 333 B record 开销。4 KiB 探针栈下第一笔成功，第二笔失败，`uxTaskGetStackHighWaterMark` 显示探针任务自身最低未用 **2,356 B**。此前 8 KiB 探针栈在 `efrp_start` 后仅有 18,540／17,408／18,540 B，第一笔 **16,717 B** 即失败，caps 同为 2052，启动低水 12,984 B。两次 `tls_error=0` 仅表示尚无 TLS 库错误，不表示握手或信任成功；两次均未进入 `TLS_HANDSHAKING`、未创建 FRP session、未发送 Login／注册或 Pong。旧锁在 TLS 后失败于 18,872 B session 的历史实验，不可代替当前锁结论。

另将仓外任务栈降至 **3,072 B** 后，三次签名 QEMU 均到达 guest `RUNNING`、Base `READY` 和 DHCP；自身最低未用分别为 1,340／1,332／1,340 B，仍高于 1 KiB 门。三次测试时间回调均未在 10 秒窗口内返回 true，止于 `clock_invalid`，没有创建 FRP 客户端；其中一次显式 QEMU `-rtc base=2026-09-27T10:00:00,clock=host` 仍同样停止。本记录不推测该 QEMU RTC／`time(NULL)` 差异的原因，也不将 3 KiB 的未到达阶段外推为容量结果。未继续缩小栈。

## 原始收据与边界

`mac-work-1:/private/tmp/esp-frp-current-session-20260927/attempt-*/` 保留每次原始 `qemu-uart.log`、`qemu-process.log`、`frps.log`、命令 JSON、4 MiB 合成 Flash、独立 efuse 副本和官方验签输出。关键 UART SHA-256：8 KiB 容量失败 `77c5cd2d85b92897f4c1ed08752a1266b41d55f879c9c58c4a02fd4c05b316d8`；4 KiB 容量失败 `065d279f6e9b732eb91aa0c3cba3cff7dcdd2995c431559bcc8885a26df53333`；3 KiB 原始／复跑／固定 RTC 分别为 `fc6e1f39613ad0205fe95807b149bd56bd54b2ead9d5ea76710ae25f2a6ccf40`、`b4dae7c98d27a0ca0cee39bc2737baf0e31cab279a0dd17f57efed76191c8f25`、`59de049674e42be4c22d22ae992862dd877e4c5ef97dc1bbf51fd01789137573`。8 KiB 与 4 KiB 镜像完整 SHA-256 分别为 `9e8b7bb54abb6bfc2bc9000233a680a7bc13036721ed70433488e1ab49334236`、`d32f465e0b23d834d104c370ba87020ac25a5f04b4207254bcbcf8fca5849ca1`；三次 3 KiB 为 `d326518ab6f10f6d5aa65f99b8fbc2953d498b79ad87d4adca9ab89427db8e59`。FRPS 每次均有 `READY/STOPPED`，退出后无 29372 端口监听。

上述原样静态 TLS 配置实验只确定**当前锁、测试输入与该网络负载下，在 TLS 握手前的内存失败**。正式 SNTP、证书验签、FRP 会话及 Flash 加密记录、MQTT Broker TLS、OTA HTTPS、五能力同时运行和实体板资源门均未完成。没有降低 48 KiB free、24 KiB largest 或 1 KiB 栈余门；P6-03 继续开放。

## 仓外固定时钟与 SDK 动态缓冲对照

为去掉上段 QEMU 时钟偶发性，另一组**纯仓外**测试探针在 DHCP 后调用 `settimeofday` 设置 `2026-09-27T10:00:00Z`。两份签名镜像使用同一 4,096 B 探针栈、同一测试包／证书／FRPS、相同 Base／五仓锁、相同探针 C 源与 QEMU 命令；源码 SHA-256 分别为 `frps-probe.c=511bad5dcc9fa4db077ea49272a6e650cfa466a5ef842d1f51a450cbde148f20`、`base-main.c=0372e93f95c46f5df68b13abc9c6beb0f2cdd4ccc90b3d729eca6a25f03b15d4`。每次都以新签名 app 摘要重建 ECS2，并经官方 ECDSA v1 验签。`sdkconfig` 唯一启用项差异是 `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`；Kconfig 同时展开一个仍为 `not set` 的 `CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA` 可见项。入站／出站内容上限都保持 16,384／4,096 B，未改变产品源或正式配置。

| 同输入阶段 | 静态缓冲，`sdkconfig` SHA `20ba69bf6456e6a71fc6362c325e380ef287c082b425b0ce950be41f81f1b97a` | 动态缓冲，`sdkconfig` SHA `1a4a148b1e23bd6c197e7dc4abda2a44ea3590a1e9244922b6d61d8964baea4d` |
| --- | --- | --- |
| 签名 app SHA-256 | `efebf1feb2a3776cc8eb9e9192cca0007746093fd4388eb07af36ffb3788b641` | `456b400cea4705427e1a7d9f703780f02583437c652982846270781adbcdc91c` |
| guest／网络／时间 | `RUNNING`／Base `READY`／DHCP；`base_sntp_ready=0`、探针时间 `1790503200` | 同左 |
| `efrp_create` 后 free／largest／minimum | 22,756／20,480／22,628 B | 22,756／20,480／22,756 B |
| `efrp_start` 已排队后 free／largest／minimum | 22,036／20,480／18,888 B | 22,036／20,480／22,036 B；相邻 OpenETH 采样已到 20,428／18,432／18,888 B |
| 最后阶段与失败 | `failure_phase=CONNECTING(1)`，4,429 B/caps 2052 分配失败，minimum **460 B**，证书验证尚未发生 | 进入 `TLS_HANDSHAKING(2)`；随后 `failure_phase=2`，1,024 B/caps 6144 分配失败，minimum **320 B**，`tls_error=0`、`verify=0` |
| 收敛结果 | `FAILED=8`，没有 FRP session | `FAILED=8`，`ready=0`、`pongs=0`，未进入 `AUTHENTICATING`、没有 Login／注册 |
| 探针自身最低未用栈 | 2,316 B | 2,236 B |

当前 FRP `client.c` 只有 `efrp_tls_step` 返回成功后才调用 `efrp_session_create`，并在后者成功时才把阶段切到 `AUTHENTICATING`。动态缓冲运行的 `verify=0` 与 `tls_error=0`、随后 `failure_phase=2` 且 `EFRP_NO_MEMORY=-20`，据此可确认严格 TLS 握手及证书检查已成功，**下一步建立 session 时容量失败**。现有失败回调没有调用栈；虽然 FRP 源码中 Yamux 控制流 ring 的申请也是 1,024 B，本记录不将那笔失败强行归因给它。两份镜像均未完成 FRP Login 或进入 READY。动态缓冲只改变本仓外实验的停止点，仍未满足 48 KiB free／24 KiB largest 门。

原始证据分别保留在 `mac-work-1:/private/tmp/esp-frp-current-session-20260927/attempt-fixed-clock-static-4096/` 与 `attempt-fixed-clock-dynamic-4096/`，包括合成 Flash、官方验签输出、未脱敏 UART、FRPS 日志、输入 `sdkconfig`、探针源码与 QEMU 命令；UART SHA-256 为 `c667f5f435241a19f1a5d397c93fb237cfda783e5335478ff248be3099f25a44`／`a4f256dab614c84a65fad79de75e0d7c4327fc72f897b94f8deb782def408f24`。两份官方本地 FRPS 均 `READY/STOPPED`，退出后端口不再监听。`settimeofday` 是用于诊断内存的仓外时间输入，Base 正式 SNTP 门仍为 false；本对照不计入五能力并发或设备级验收。
