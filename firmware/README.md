# ESP Base 固件

生产主应用为 `apps/esp_base/main`，原生消息计数直接由控制 owner 执行，复用现有认证 MQTT 消息输入。固件 OTA 独立于业务包，使用 V4 收据、精确签名固件身份、唯一升级 claim、短 Flash I/O 仲裁和本地 30 秒启动确认。设备 FRP 的流式上传与本机 USB 命令触发的 HTTPS URL OTA 共用同一校验、备用槽写入和恢复链。ESP Tool Mac App 的软件已收敛为本机设备管理，内置 FRP／frpc 与远程 Bridge 接线已删除，实际安装／USB调用链仍待验；固件自身的 FRP／MQTT 能力保持。

## 源码与目标

- `components/native_business`：有界消息字节计数、状态、暂停／恢复和 100 ms 定时状态；不新增业务线程或队列。
- `components/device_protocol`：USB／MQTT／FRP 严格协议、身份／boot／期限／请求守卫、原生结果和 OTA owner。
- `components/ota_operation`：182 B V4 固件收据、签名槽观察、写前意图、恢复与长／短存储占用。
- `components/remote_config`、`device_identity`、`wifi_runtime`、`time_runtime`：既有配置与设备事实。

C3 使用 USB Serial/JTAG VFS；ESP32 使用 UART0。控制任务栈 8,192 B，主任务至少 6,144 B。签名 C3 使用 RSA-3072 v2，ESP32 使用 ECDSA P-256 v1；目标、签名方案和几何各自核对，软件测试键不属于正式信任。

ESP32 UART0 使用锁定 SDK 的 RX 驱动与 IRAM 中断，接收环请求 9,217 B，容纳一条 9,216 B 上限命令及换行。容量输出、验签及 Flash 停用 cache 时仍由 SDK 中断接收；控制 owner 保持非阻塞读取、原命令期限和解析上限。没有新增 TX 环、事件队列或任务，实际驱动对象、内部 RX 存储、同步对象和 IRAM 代码均计入容量；这不提供硬件流控或无限连续输入保证。C3 保持原 USB 背压路径。该改动须经真实串口、同存资源和完整候选验收，宿主初始化回归不替代实体结果。

TLS 使用 SDK 原生固定 RX／TX，内容上限保持 16,384／4,096 B。动态 adapter 按首 record 分配，而同一次握手步骤可以在 core 内重组后继 record；其分配长度与 core 的固定写入上界不能等同。固定分配让完整输入区贯穿握手及后续读取，CA／主机名、可信时间、加密套件、超时及镜像验签合同保持。每连接 RX／TX 的真实普通申请分别为 16,717／4,429 B，不能把内容上限当申请字节数。C3 使用 `INTERNAL|8BIT`；ESP32 使用锁定 SDK 的 `MBEDTLS_IRAM_8BIT_MEM_ALLOC`：达到 RX 或 TX 内容阈值的大块申请优先 `INTERNAL|IRAM_8BIT`，失败时回落 `INTERNAL|8BIT`，较小申请仍使用 DRAM。该规则也可能覆盖达到阈值的其他 mbedTLS 对象，不能把它缩写为仅搬移 TLS buffer。SDK 的非对齐访问异常处理有时间成本，必须按实际握手期限与两个内存能力域重新验收。MQTT、FRP 与 URL OTA 三条 TLS 的缓冲请求合计 63,438 B，连接、握手、证书和 crypto 对象另计。该成本随新配置重新构建、签名及实板容量验证，不能继承动态缓冲候选的资源结果，也不加回观测成本。 ESP32 idle 栈设为 2,048 B；此前 1,536 B 配置的实板最低余量 972 B 未通过 1,024 B 门。增加配置值不证明新候选栈门已通过，仍读取真实任务实例 HWM。

Base 自有的固定 Flash 观测保留每 boot 的 OTA／FRP／配置短 claim 计数与实测时间；OTA 的 retire／prepare／select 使用原 operation_id、request_id 和 boot_id 标记边界，FRP 活跃／备用数来自同一次既有 SDK 状态快照。独立 UART 行位于容量 END 外，沿用既有五秒报告与主动重启点，不改 MQTT／FRP／OTA 公共 API 或业务 JSON，也不增加任务、定时器或动态分配。具体字段、成本与不可推导的验收范围见[宿主工具的观测说明](../tools/README.md#正式容量统计)。

FRP OTA 先完成来源验证与 inactive 退役，再开放正文上传；客户端通过原 ID 的 accepted／running 区分准备与接收。时序问题、软件验证及仍待处理的控制响应时延见[上传准备说明](../docs/issues/frp-upload-before-retirement.md)。

锁定的 FRP 0.2 修复将 scratch 恢复和新记录擦除拆为 16 个 4 KiB 短 claim，每扇区释放后阻塞两个 tick，再申请下一扇区；全部擦除成功才发放 scratch lease。Base 的 500 ms Flash 获取预算、OTA 任务优先级与阶段期限保持。逐扇区故障和真实 FreeRTOS 调度的软件对照见 [FRP 擦除争用记录](https://github.com/darren-you/esp-frp/blob/0d9c08be7122ad73b0c24d912cf9b92a540ae3b2/docs/issues/c3-scratch-erase-owner-starvation.md)；新依赖必须重新构建、签名并验证双板实际联合运行，不能继承旧候选的容量结果。

[C3 分区](partitions/c3-partition-table.csv)和 [ESP32 分区](partitions/esp32-partition-table.csv)均为双 `0x1e0000` app，移除业务包区。scratch、Base NVS 与 ESP32 旧 AT 区保持各自真实定位；新布局必须通过一次性有线装配迁入。旧 V3／损坏收据不能当作空状态，普通 app OTA 不能迁分区表。

## 构建

产品版本的唯一声明为仓库根 `release.json`，普通 `esp_base` 构建由
`tools/build_identity.py` 生成 `PROJECT_VER` 和只读构建描述，不能由另一个 CMake
常量覆盖。描述位于 SDK 规定的首个 DROM 段、`esp_app_desc_t` 后的固定偏移，
包含完整 64 位构建号、目标、源码 SHA、输入摘要和 run。它不复用
`secure_version`，不写 eFuse。

普通 `idf.py build` 的描述为 `local_validation=true`、构建号 0；它不分配正式编号。
正式身份只接受显式 `FAST_DEPLOY_PACKAGE_CORE_ROOT` 和同次 Core 冻结快照，
release unit 为 `esp-base`，原生 project name 继续为 `esp_base`，
两目标分别为 `esp32c3/riscv32` 和 `esp32/xtensa`。run 最长 127 个 ASCII 字符；
超过原生容量会拒绝，不截断。部分正式环境、LAB 后缀、目标或源码不符均不能回落本地身份。
每次编译前核对生成文件，`app` 构建结束后从实际 `.bin` 固定偏移回读全部字段，
启动时输出 `ESP_BASE_BUILD`。现有 `firmware.status` 业务协议字段保持原合同。
这些是身份接线与字节校验；正式独立包、签名资格、持久防退和旧设备迁移须分别验收，
本地标记与构建号 0 不授予正式交付资格。

先读取 [SDK 与宿主工具](../tools/README.md)，在独立官方基线装配 schema 2 的唯一受管容量统计派生，并导出 SDK 环境。显式 target、sdkconfig、defaults 与 build 目录，分别解析两目标锁。组件版本变化必须让官方 Component Manager 重新解析实际目标 lock；`update-dependencies` 默认 lock 名不能代替 ESP32 自定义 lock 的核对。

```bash
idf.py -C firmware -B /absolute/build-c3 -DIDF_TARGET=esp32c3 reconfigure
ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_host_tests.sh
```

ESP32 自动签名构建必须使用仓外绝对路径 P-256 键，启用 signed boot/update、build signed binaries 与 rollback。无签名只读软件探针须显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON`，不能作为刷写制品。C3 自动签名同样在独立签名 defaults 提供仓外 RSA 键。正式制品还需官方验签、精确 signed bin 摘要与分区容量核对。

外部签名准备使用官方 `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`，保留 ESP32 的 ECDSA 签名启动与更新、C3 的 RSA 签名更新以及双目标 rollback；两目标硬件 Secure Boot 均关闭。ESP32同时要求仓外绝对路径、非链接普通64B的 `CONFIG_SECURE_BOOT_VERIFICATION_KEY`，直接消费本轮已核对的公开P-256验证字节；不能启用offline probe代替该装配。此路径不读取私有签名键，只产unsigned app／partition输入，须再由受控签名事实源独立签名、验签和核对完整signed身份后才能交付或发送OTA。自动签名 `y` 的原私有键合同保持，未签输入不具有消费资格。

精确公开依赖来自 [唯一组件清单](components/device_protocol/idf_component.yml)与 [C3](dependencies.lock)／[ESP32](dependencies.lock.esp32)锁。host 使用这些已解析组件，不从相邻源仓运行时 import。构建图只读取当前锁定的 MQTT／OTA／FRP 与 cJSON，不读取退役运行组件。

普通 `esp_base` 产品在本目录唯一 CMake 入口按 SDK `http_parser.h` 公开的编译合同设置 `HTTP_MAX_HEADER_SIZE=8192`，限制 HTTPS URL OTA 的响应头计数（包括状态行），不修改 SDK 源码或派生 recipe。parser 可能因接收分片的计数提前拒绝，所以 8KiB 是上界，不保证任意 8KiB wire 头都可接受。当前正式配置关闭响应头保存与 Content-Range 解析；双目标冻结须核对这两项及实际 `compile_commands.json` 中 parser 的唯一宏。URL 语法解析不使用该头计数；FRP 流式固件正文不走此 HTTP client。

本轮完整输入、失败记录和签名结果见[原生软件检查点](../docs/operations/native_software_checkpoint.md)；实板及正式交付边界见[执行计划](../docs/operations/ota-allocation-diagnostic-checkpoint.md)。

容量帧 schema 2 在 BEGIN 前通过现有 control owner 取得 emqtt 固定 copy，BEGIN 后输出唯一 `ESP_BASE_CAPACITY_MQTT` 行；不增加任务、队列、业务指令或 SDK 统计补丁。该行记录同一未完成 DATA 回调的真实完整 owner／partial 请求字节与 outbox wire tuple、采样起止区间和原 FULL／notice 失败历史。80 B copy 栈及 emqtt 固定观测／锁成本由新候选实际 ELF／map和栈HWM承担。满输入 tuple、一次 FULL 和数值门均不自动证明最大合法 outbox、认证消费、OTA真实重叠或 R5／R6；观测与签名候选、配置、同 boot 原始进度及严格 TLS peer 原报文须另行对账。

工程结构、工具链、签名制品和设备写入边界遵循[Embedded Firmware 工程标准](https://github.com/darren-you/darren-space/blob/master/harness/docs/workspace/standards/embedded-firmware/embedded-firmware-golden-path.md)。
