# P6-03：Base 配置提交单缓冲检查点

2026-09-28。Base 主固件在 `esp-frp@b462c1497438cfeb514f022bc2d2d6c026599a41` 及其 C3／ESP32 官方依赖锁不变的前提下，收缩 v3 配置 NVS 工作区。原 `s_load_bytes`、`s_commit_bytes` 各为 `EBASE_CONFIG_MAX_BYTES=7618` 字节，常驻 `.bss`；启动读取、控制任务配置指纹、提交写入与提交后读回均由同一个 Base 控制 owner 串行执行。写入前编码字节交给 `nvs_set_blob`，写入及 commit 成功后由同一缓冲读取并解码持久值，写入字节无需继续占用第二块缓冲。

提交后的检查仍要求完整读回：v3 解码器校验头部每个字节、保留位、精确载荷长度和各字段值，合法配置在 wire 上只有一个规范表示。读回模型逐字段与原候选比较，并要求 revision 精确为 `expected_revision+1`；比较不依赖 C struct padding。缺失、无效或不同的读回仍返回 `ESP_BASE_CONFIG_UNCERTAIN`，不改调用方已提交配置，也不自动重试或擦除 NVS。工作模型和共享字节缓冲在返回前擦除。配置 schema、单键、最大 7,618 字节容量、控制协议与密钥来源均未改变。

宿主 ASan/UBSan 配置存储定向回归通过，覆盖完整 MQTT／FRP 配置成功提交，以及 revision、Wi-Fi、MQTT 主机名、FRP 主机名被改写后的不确定结果。隔离 `mac-work-1:/private/tmp/esp-base-frp-status-consumer-20260928/` 的 Base 双目标 host 全套分别 **20/20／19/19** 通过。固定 ESP-IDF `578cf89c`／lwIP `2758df4` 的双目标普通完整构建及官方 app 分区尺寸检查通过；C3 未签名 app `0xdf120` B、SHA-256 `e0a27c7e778d016cf1582ac927541be8485ca3e4b0e575cf298a6b21b1e6e59a`，ESP32 显式离线探针未签名 app `0xd2ce0` B、SHA-256 `21cb50f062be22ccb5e6bca9cd80307237ab3a14b8c9dd9fb0c34a7a38854e49`。两目标 ELF 均由原先 `s_load_bytes` 与 `s_commit_bytes` 各 `0x1dc2` B 改为唯一 `s_config_bytes` `0x1dc2` B，故各减少 **7,618 B** 常驻 `.bss`；没有改任何依赖锁或正式分区。

后续在 `mac-work-1:/private/tmp/esp-base-p603-config-single-20260928/` 只把本次 `esp_base_remote_config.c` 复制到此前成功登录官方 FRPS 的[ESP32 IRAM 仓外诊断](esp32-p603-iram-capacity-qemu-checkpoint.md)副本。两份 `source/` 逐文件比较，除这一源文件外相同；该输入仍是旧 FRP `0af1220` 的仓外 IRAM 会话变体、固定测试时钟、OpenETH、额外 4 KiB 探针任务和 64 KiB ABI 2 签名 guest，**并非当前 Base／FRP 正式产品配置**。重新构建的 ECDSA v1 app 为 `0x10fff4` B、SHA-256 `6ff2ba38fe3cbbe2733620158c80996a77d61dc1bbde8246f0dc09150c559359`，官方 app／分区验签及尺寸门通过；按新 app 摘要重新生成 ECS2 SHA-256 `ab1b9a795fff12c7a94b593910c9e505c1669b667bfdb88e2e7400c0b1927c9f` 与全新种子 Flash SHA-256 `022642451be9b44ecbb0276dfc77744a9c4810b40b728813f0edffb9aae96118`。

同一镜像和种子分别独立运行两次，官方 FRPS 登录、代理注册和 Pong 均成功，`ready=1`。第一次普通 8BIT 堆历史最低 **40,508 B**、就绪时空闲／最大块 **49,156／47,104 B**，原始 UART SHA-256 `7d91a85599eed9772293ba2ca8a31cfca5fc187c20ecd4a7790c72269ca1015d`；第二次分别为 **38,968 B**、**47,584／45,056 B**，UART SHA-256 `53ebb3b22d81bff52576692c6d36bc46631abe29cce84f4ad0fd55fcaa57efc0`。两次就绪 IRAM 空闲都为 **68,308 B**。旧同源诊断单次最低 **32,100 B**；静态 `.bss` 精确少 7,618 B，网络运行的最低值改善 **6,868–8,408 B**，不同运行有分配时序与碎片差异，不能把差值全归为代码的固定收益。QEMU 使用旧 FRP 状态复制实现，`verify=UINT_MAX` 是状态快照缺陷；实际 TLS 握手仍以严格 CA／IP SAN 验证进入会话。

较低一次距 **49,152 B** 门仍差 **10,184 B**，而且没有 MQTT／OTA 同存、正式 Base FRP owner、真实 Wi-Fi 或 C3 对应网络输入。此 QEMU 只验证该隔离组合的容量方向；实体 NVS 掉电、两板完整五能力和 48 KiB 资源门仍未验收，不把这个读数用于正式产品验收。
