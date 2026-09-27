# P6-03：新 FRP 锁的 ESP32 FRPS／TLS 容量诊断

2026-09-27。在[同锁双目标无网络签名启动](p6-03-frp-phase-union-current-lock-qemu-checkpoint.md)之后，以 Base `1fe24302f5ec94dfc30180eb86369dbdc62774b8`、FRP `0af12209ee731617e635684309c026ae6b49c5ae` 和固定 IDF/lwIP 另建 ESP32 仓外诊断镜像，证据位于 `mac-work-1:/private/tmp/esp-base-1fe2430-frp-0af1220-qemu-20260927/`。两份诊断均由同一精确 Base 归档独立解出、官方 Component Manager 重新取得受管组件；六组件内容重算与锁一致，五个 Git 组件按精确提交逐文件一致。仓外 scratch 候选分区、真实签名 ABI 2 包、测试 CA/证书、官方 FRPS v0.71.0、OpenETH 和严格 CA/IP SAN 验证条件相同；未使用旧 app、ECS2、Flash 或此前 QEMU 结果。

这两份镜像都另加 **4 KiB 测试任务**和固定测试时钟回调，在 QEMU OpenETH DHCP 后直连本机官方 FRPS。原样 Base 的本次启动 SNTP 门没有获得同步，正式 FRP owner 因而不具备启动条件；探针调用 FRP client 只能诊断库和资源路径，不能充当产品入口。相同 Base 源码归档 SHA-256 为 `e79401d9ae3561bd54dfe7f254d7af070ca75391c7bf0eee3f6375b6ef0ccc9c`，`source-diff-audit.json` SHA-256 `f97ab1bf76464a9ac9b3ad44df8ea696a06b158f20ede63019586c9d19e8a536`：相对原样 Base 仅改仓外 `esp_base_main.c`、该 app 的 CMakeLists，并新增探针、测试 CA 和配置四个文件；静态／动态变体的源码逐字节相同。两变体均经官方 ECDSA v1 app／签名分区表验签、候选分区解码和容量检查，签名长度均为 `0x10fff4`，双槽各余 `0x1000c`；按各自新 app **完整**摘要独立生成 ECS2 sequence 6 及全新 4 MiB Flash，构建后再审计六组件与五 Git 源文件。运行结束均已退出 QEMU/FRPS，Flash 产品包和 NVS 的独立读回、NVS CRC 与 scratch 恢复检查通过。

| 项目 | 原样静态 TLS 缓冲 | SDK 原生动态 TLS 缓冲 |
| --- | --- | --- |
| 唯一主动配置差异 | `CONFIG_MBEDTLS_DYNAMIC_BUFFER` 关闭 | `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`；Kconfig 额外展开未启用的 `DYNAMIC_FREE_CONFIG_DATA` 项 |
| 最终 sdkconfig SHA-256 | `62bc62b50a77ccbc5576ee367fdb360ea57e4077053a6bfed4c08e2f7d021e50` | `c6974f4b88a5a36145786c9ac580d0082930b414b957de3d038614a49ffa2da5` |
| 签名 app SHA-256 | `07409a2a85ad1f5647355eee724754f7a5f9252dfc3133adf709b8515bb92ed8` | `5beac90e23f1f6337b4864faee9409e954f485665f3ee0ad8d3cb2ff5e9751c2` |
| ECS2 SHA-256 | `255dd095f1571024bd3307215fd7f013dc47dc01c5687df58d905cc09a7dca6b` | `e43cb89a58a55360b146db19964a9c76890108ccc78727982089703989e5f5b3` |
| 全新种子 Flash SHA-256 | `abc3b4b368e807f3d5b3cbe0d99ac7298af09f13718371b72a8fc02fc46ea1b4` | `7fff9e8111131d7db607cdb37492c30c7830bb220a0b7fa5c72855b91283c2e8` |
| 客户端最后可证阶段 | 三次尝试均在 `TLS_HANDSHAKING` 超时，`EFRP_TIMEOUT=-9`；`tls_verify_flags=UINT_MAX`，尚无验签结果 | 严格 TLS 验签通过，`tls_verify_flags=0`；进入 `AUTHENTICATING` 后 `EFRP_NO_MEMORY=-20` |
| 整次启动内部 8BIT 堆历史最低空闲 | 1,020 B | 1,524 B |
| 失败分配证据 | 跨重试累计 182,370 次，最后一笔 1,522 B；不能据此推断单次泄漏或每次失败尺寸 | 本轮最后可见 4,437 B／caps 2052 的失败分配；调用来源未定位 |
| 原始 UART SHA-256 | `6ca9818311f522bd517ed920700121931ec5fa8b0daccd17cc7227eb55ef6024` | `818f53f45e70d21fd283142631a73e543987405282ef056c2862dc3c1d41c77b` |

静态输入在 Base `READY` 后获得 OpenETH DHCP，进入 TLS 握手，但三次重试都没有完成验签；本机 FRPS 日志 SHA-256 `7a2f4b332cfa54b705bbc8a8e8d97444a06d59e7a03e198d2b7801d1803d2b30`，仅能证明服务端启动，不能证明收到 Login。动态输入保持探针源码、任务、时钟、CA/IP SAN、FRPS 和候选 CSV 不变，仅打开 SDK 动态 TLS 缓冲；其验签成功与 `AUTHENTICATING` 阶段由客户端状态证实，但没有 Login 到达服务端、注册或 Pong 证据。4,437 B 分配失败不能凭大小归因 FRP、Mbed TLS 或 lwIP；当前没有对应调用栈。两输入 `ready=0`、`pongs=0`。

另用**同一动态签名 app**和独立新运行 Flash 尝试在首次 4,437 B 分配失败处用 GDB 条件断点读取调用栈，该尺寸没有复现；本轮首次 `AUTHENTICATING` 后先以 `EFRP_TIMEOUT=-9` 回退，第二次尝试返回 `EFRP_NO_MEMORY=-20`，可见的最后一笔失败分配为 392 B／caps 6144，启动最低空闲 496 B，TLS 验签仍为 0。这个运行只证明停止位置和失败尺寸受本轮时序影响，不能拿第二次的 392 B 解释第一次的 4,437 B，也没有找到唯一根因。原始 GDB 重跑 UART SHA-256 `9d1dc4411cfc1a6476dc8648943dcfe4538eac36dd8504325f2f42a39cac396f`，GDB 条件等待日志 SHA-256 `bedd710f798610e53fbaab320bdb0a6de73309710510ecabd65e1e02308cc5b5`；本机 FRPS 日志单独保留。

固定 SDK 所含 `espressif/mbedtls@a2b3207` 的动态 TLS 发送缓冲源码会申请 `4096+333+8=4437` B，FRP 认证阶段存在经 `efrp_session_step`、`efrp_tls_step` 到 `mbedtls_ssl_write` 的可达路径，因此它是同尺寸**候选**。该 SDK 申请失败通常返回 `MBEDTLS_ERR_SSL_ALLOC_FAILED`，FRP 解释为 `EFRP_TLS_ERROR`；本次观察到的终止错误是 `EFRP_NO_MEMORY`。没有同一次分配失败的调用栈和首次错误链，不能把这个数值匹配当作已定位原因，更不能据此改 FRP 或 SDK 行为。

两份诊断镜像都没有达到 48 KiB 内部堆历史最低空闲门，也没有 MQTT、OTA 和真实产品 FRP owner 同存。静态超时和动态内存失败是各自签名输入及额外任务条件下的停点，不得与无网络原样产品读数机械相减或外推到实体板。正式 ESP32 分区、SNTP、FRPS 注册、Broker、HTTPS、五能力并发及实板迁移均未验收；P6-03/P7-02 继续开放。

仓外 `final-machine-receipt.json` SHA-256 `719e645d2b6bc151397e73bcb055d2f51e24b02ed83608386d015080999f034c` 汇总 Base/FRP/双锁、六组件与 Git 逐文件审计、输入配置、签名 app／ECS2／Flash、两次 FRPS 运行、GDB 重跑与 Flash/NVS 读回；所引 51 个原始文件摘要已复核。正式分区 CSV、签名键、实体设备与 Base SNTP owner 未修改，QEMU/FRPS 进程及 29372 监听已清空。
