# ESP32 产品验包工作区移栈后的 FRPS／TLS 容量检查点

2026-09-27，P6-03 仓外窄测。输入固定为 Base 6ef7a028c2a5e16ea62fab6a0942bf7500479cd9（同步验包工作区已移入产品 pthread 栈，栈下界 16 KiB）、FRP 9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3、MQTT c0677e5e779c3e51e814f2920420be7ec54f1d88、OTA d98361f348e19e965efd7462277dde0ae13056fa、Container 6ef74faabb675bce0180570f5bdf0232af11106a、WAMR c10736fffdf26d7c2ae234e05aa712df112eb6bf，以及固定 ESP-IDF 578cf89c343e388db43ba1f4ddcd602fedcb763c／lwIP 2758df4cd3666b3b2a5b53830148379326425c0d。Base 归档 SHA-256 为 56d3d6ce2d4c1da6c51d620eaa9028bc8f3d900b48611f5c0dbea32702ee1d01，ESP32 生成锁 SHA-256 为 5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8。

**受管源码一致性补验（2026-09-27）**：因另一份移栈签名 guest 目录发现 `P603_SAMPLE` 插桩，另对本检查点三份镜像的仓外输入做严格核对。用固定 IDF Component Manager 重新计算六项 `managed_components` 的实际目录哈希，全部匹配 `dependencies.lock.esp32`，并非只读取 `.component_hash`；Container 全目录与锁定 `6ef74fa` 源树逐文件相同（仅多 IDF 生成哈希文件），`slot_runtime.c` 双边 SHA-256 均为 `4a1699dacace223c92da2b30542fb7b16486e7612d49fb6c285586fb1e68a7a1`。Base 产品入口与本仓移栈源码相同；当前组件、Base 入口及三份签名 app 中均未发现 `P603_SAMPLE`。首建日志与后续同一树重建记录一致。严格核对原始日志为 `mac-work-1:/private/tmp/esp-frp-workspace-stack-session-20260927/managed-components-strict-audit.log`，SHA-256 `64275d6e6c989121ed79ea2cd9292ccc5d3771da158908584372505e0b6a91f3`。本补验不重跑 QEMU，只确认下文三份仓外镜像的源码边界；它们仍使用额外探针和测试时钟。

所有改动仅在 mac-work-1:/private/tmp/esp-frp-workspace-stack-session-20260927/ 的无 Git 仓外副本。签名 ABI 2 counter 包 SHA-256 为 9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e；每次按新签名 app 的完整摘要重建 ECS2 sequence 6 confirmed 绑定，填入合成 4 MiB Flash。候选几何含独立 64 KiB FRP scratch 和六页 NVS，CSV SHA-256 为 0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1。三次 ESP32 ECDSA v1 测试键签名 app 均经官方 espsecure verify-signature --version 1 验证，大小 0x10fff4 B；双 0x120000 app 槽各余 0x1000c B。没有实体设备、正式分区或产品凭据写入。

仓外额外启动 **4,096 B 测试任务**，接 OpenETH DHCP（10.0.2.15），用公开 efrp_create → start → get_status → destroy 与本机回环官方 github.com/fatedier/frp v0.71.0 FRPS（127.0.0.1:29372）交互。FRPS 强制 TLS，临时 CA SHA-256 为 84eaa94649239e6d406f906dbc7a2ac4a3f47f2783ec4b7c8e3adae8be26818d，服务端证书含 QEMU 主机 10.0.2.2 的 IP SAN，客户端仍配置 MBEDTLS_SSL_VERIFY_REQUIRED。三次 guest 均到达产品 RUNNING 与 Base READY。仅测试探针在 DHCP 后以 settimeofday 设置 time(NULL)=1790503200，仓外回调检查 2024—2030 年区间；FRP 自身的原生 time(NULL) 判断仍执行。每次日志均显示 base_sntp_ready=0，即 **正式 Base SNTP 门仍 false，正式 FRP owner 不具备启动条件**。该探针时间不构成 SNTP 验收。

## 原样静态配置：独立基线

第一次使用未加 worker 栈采样的原始 4 KiB 探针，源 SHA-256 为 511bad5dcc9fa4db077ea49272a6e650cfa466a5ef842d1f51a450cbde148f20、静态 sdkconfig SHA-256 为 20ba69bf6456e6a71fc6362c325e380ef287c082b425b0ce950be41f81f1b97a，签名 app SHA-256 为 72fb03c4f2196b4bd9d715c60e9624f3d9280dc3ffadd08d3f4a723afcdb6ae6。CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384、OUT_CONTENT_LEN=4096，SDK 动态缓冲关闭。guest 首次探针开始时 free／largest／启动以来 minimum 为 **53,004／43,008／47,748 B**，启动低水比 48 KiB＝49,152 B 门少 **1,404 B**；DHCP 后为 39,148／36,864／33,668 B。

| MALLOC_CAP_8BIT 阶段 | free／largest／minimum（B） | 结果 |
| --- | ---: | --- |
| efrp_create 前 | 38,972／36,864／33,668 | 仓外测试时间通过，正式 SNTP false |
| efrp_create 后 | 28,492／27,648／28,228 | STOPPED，worker 已分配，但未采样其栈 |
| efrp_start 排队 | 27,784／27,648／24,624 | CONNECTING(1)、attempts=1 |
| FAILED(8) 阶段采样 | 26,540／25,600／**2,632** | failure_phase=CONNECTING(1)、EFRP_NO_MEMORY=-20 |

分配失败回调记录 **2,212 B，caps=2052**。verify=4294967295 是尚未验签哨兵值，tls_error=0 也不能证明 TLS 成功；没有进入 TLS_HANDSHAKING。探针任务最低未用栈 **2,312 B**。这份输入没有 worker 栈读数，不能用下一份镜像的堆数代填。阶段采样发生于异步清理附近，不代表失败调用瞬间的堆快照。

## 同探针静态／动态缓冲对照

第二份探针只在仓外加入 xTaskGetHandle("esp_frp") 和 uxTaskGetStackHighWaterMark 读数，源 SHA-256 为 cbbceeb888047ebd1188d4e0d128f45c41369a322f742bda4f7231ffcaa0d881；第三份保持这份探针源码不变，仅将仓外 sdkconfig 启用 CONFIG_MBEDTLS_DYNAMIC_BUFFER=y。Kconfig 同时展开一个仍为 not set 的 CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA 可见项；入站／出站内容上限继续为 16,384／4,096 B，生产 SDK 配置未改。两份签名镜像分别验签、重建 ECS2、冷启动 QEMU；仓外测试时钟、包、证书、FRPS、OpenETH、候选几何和 4 KiB 任务栈相同。两份的仓外 Base main 源 SHA-256 均为 0372e93f95c46f5df68b13abc9c6beb0f2cdd4ccc90b3d729eca6a25f03b15d4。worker 在 FRP 端配置 6,144 B 静态任务栈；下表栈值是固定 IDF 返回的最低未用**字节**。

| 项目 | 静态缓冲 | SDK 原生动态缓冲（仅仓外） |
| --- | --- | --- |
| sdkconfig SHA-256 | 20ba69bf6456e6a71fc6362c325e380ef287c082b425b0ce950be41f81f1b97a | 1a4a148b1e23bd6c197e7dc4abda2a44ea3590a1e9244922b6d61d8964baea4d |
| 签名 app SHA-256 | f2ade348aa9ce758dda63d5a3e4893df462bf85b8c56ac4d608d6c12f32f61d1 | eccf2c2921745559dff478aee6b691398ed09ae887353919debf4f0506b5ec2d |
| 探针开始 free／largest／minimum | 52,996／43,008／47,748 B | 53,004／43,008／47,748 B |
| DHCP 后 | 39,064／36,864／33,668 B | 39,148／36,864／33,668 B |
| efrp_create 前 | 38,888／36,864／33,668 B | 38,972／36,864／33,668 B |
| efrp_create 后 | 28,408／27,648／28,228 B | 28,492／27,648／28,228 B |
| efrp_start 排队 | 27,688／26,624／24,540 B | 27,784／27,648／24,624 B |
| TLS／session 阶段 | 未进入 TLS_HANDSHAKING | 第一次 TLS_HANDSHAKING(2) 为 21,980／21,504／21,936 B；进入 AUTHENTICATING(3) 为 **5,908／5,376／5,908 B** |
| 首次失败／超时 | failure_phase=CONNECTING(1)、EFRP_NO_MEMORY=-20；2,212 B／caps 2052 分配失败；DRAINING(6) 采样 27,984／27,648／**2,544 B** | 首次 failure_phase=AUTHENTICATING(3)、EFRP_TIMEOUT=-9；BACKOFF(7) 采样 28,172／23,552／**944 B**，verify=0、tls_error=0；当时失败回调累计 173，最近一笔 1,532 B／caps 6144 |
| 后续／结束 | FAILED(8) 采样 28,044／27,648／2,544 B，ready=0、pongs=0 | 自动重试至 attempts=5；观测结束时为 TLS_HANDSHAKING(2)，17,540／14,336／944 B，ready=0、pongs=0；失败回调累计 **691**，最近一笔 1,532 B／caps 6144 |
| 最低未用栈 | 额外探针 **2,216 B**；FRP worker **4,176 B** | 额外探针 **2,104 B**；FRP worker **2,320 B** |

动态镜像第一次从 TLS_HANDSHAKING 切入 AUTHENTICATING：按当前 FRP client.c 顺序，只有 efrp_tls_step 成功且 efrp_session_create 成功才会切到该阶段。随后清理时取得 verify=0 且 tls_error=0，可确定严格 TLS 握手和证书验证成功，并已创建 FRP session。AUTHENTICATING 阶段快照仍显示 verify=4294967295，是客户端仅在 drain 时复制 TLS 状态的采样时机，不能把它当作证书失败。首次认证阶段因 EFRP_TIMEOUT=-9 退避，未进入 REGISTERING 或 READY，没有认证 Pong。FRPS 本次错误级日志只记录 READY/STOPPED，没有 Login 收发事件；现有证据**无法判断 Login 是否发出、FRPS 是否收到或是否回复 LoginResp**。

动态运行的 173／691 是失败回调的**累计采样计数**，跨多次重试与清理；1,532 B 仅为各次快照可见的最近一笔，不是首次失败尺寸，也不能由此推断单次泄漏或认证超时根因。minimum 944 B 是整个启动以来的堆低水，不等于 AUTHENTICATING 初次进入时的 free。采样和日志本身改变镜像、内存与时序；表内静态／动态值只在这两份同探针镜像之间比较。

## 原始收据与裁决

mac-work-1:/private/tmp/esp-frp-workspace-stack-session-20260927/attempt-static-original-4096/、attempt-static-worker-4096/、attempt-dynamic-worker-4096/ 分别保留原始 UART、FRPS、QEMU 命令与进程日志、4 MiB 合成 Flash、独立 efuse 副本、签名 app、验签输出、sdkconfig 和仓外探针源。三份 UART SHA-256 依次为 571515724bcbeebac04ba694a3a150011f421c33dee4c37f0646b2f661eab8e2、e9ead3895d66c284ee164d4e805d24baeca821469bf0368c35046d882539367b、9cdb4e134a7e1721434b7d3cde56f58d8b425074afdfebe8bcaab47a659588a3；对应 seeded Flash 为 c5ad57da8dabf761adc7c365ca8900561e3c1b4f97a279084400ce8090a67712、5cf33a052957214b7359be755d5940fcfb5db7c04ee0b54096bb8c6de272dbad、faa5817d7e5e5b3c185f0c11e7d71b397f4a6e182033d4f79847f274bf5e75f6。FRPS 三次均有 READY/STOPPED，结束后无 29372 端口监听。测试 runner SHA-256 为 7c30684e6437a122414b1718b27da758fc469c38043b0324d26bf43faf2e927e。

4 KiB 探针任务是额外仓外负载，**并非产品 Base 的正式 FRP owner 路径**。现有日志没有与探针创建前严格同点、同能力掩码的成对堆采样，不能从启动读数扣出精确探针成本。静态 OOM 和动态认证超时仅证明各自签名输入、QEMU 网络与测试时间条件下的停止点，不能断言无探针正式 owner 在同一点失败。三份镜像在探针开始时的启动低水均低于 48 KiB free 门，动态镜像在认证阶段的最大连续块也低于 24 KiB；不降低任一容量门。正式 SNTP、正式分区、FRP 登录／注册与 Pong、MQTT Broker TLS、OTA HTTPS、五能力并发和实体板资源均未验收，P6-03 继续开放。
