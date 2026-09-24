# ESP32-C3 小内存 Base 常驻 DRAM 复测

本记录对应独立 `codex/c3-low-memory` 分支的 Base 静态内存改动，以 `esp-base@31f5ebcc0bbc756fe5e78cb7c53f9042832ce286` 为起点。固定 SDK 为公开 `esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c` 和 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d`。本次收敛 Base 配置及 MQTT owner 的静态缓冲，不修改配置 schema、NVS 事务、分区、TLS、FRP、MQTT 或 OTA 协议，也不刷写设备。前文保留首次收敛和依赖升级时的历史快照；末节给出本分支继续复用配置缓冲后的现值。各轮均不含真实联网会话。

## 消除的长期占用

用同一五组件 ESP32-C3 链接探针的 `esp_base.map` 核对以下静态对象。前三项先由 `a0e61e3` 消除；后两项在本分支继续收敛。

| 对象 | 原占用 | 调整后的唯一数据路径 |
| --- | ---: | --- |
| 启动入口 `s_config` | 7,608 字节 | NVS 配置最终直接读入协议组件长期上下文 `s_context.config` |
| 协议控制 `s_committed` | 7,608 字节 | 条件提交成功时直接写入 `s_context.config`；提交失败保持旧值，写入结果不确定时按原逻辑重读持久配置并决定连接状态 |
| NVS `s_readback_bytes` | 7,618 字节 | 提交后读回调用结束并擦除 `s_load_bytes`，再在该缓冲编码读回值，与独立的 `s_commit_bytes` 做逐字节比较，最后擦除两者 |
| 启动入口 `s_protocol` | 7,632 字节 | 启动元数据在栈上仅占小型结构；配置在任务创建前直接载入 `s_context.config`，无需完整临时副本或堆分配 |
| MQTT owner `s_emqtt_config` 与 `s_event` | 7,716＋4,388 字节 | 两个互斥阶段使用 7,716 字节静态 `s_work` union；`emqtt_create` 在返回前复制配置，之后该空间才用于事件 |

第一批链接对齐后 `_heap_start` 从 `0x3fcb6100` 前移至 `0x3fcb07d0`，释放 22,832 字节。追加两项后前移至 `0x3fcad8e0`，再释放 12,016 字节，**相对起点共释放 34,848 字节的初始 8-bit 堆空间**；`.dram0.bss` 从 `0x22980` 经 `0x1d048` 降至 `0x1a158`。MQTT union 的逻辑共享量为 4,388 字节，实际链接减少 4,384 字节，差异为对齐。签名五组件镜像仍为 `0x121000` 字节。NVS 加载和提交继续由启动线程、随后唯一控制任务串行执行，提交候选与读回值始终位于两个独立缓冲；失败时不擦除持久配置，也不把未知结果报为成功。

启动配置现在由 `esp_base_protocol_load_config` 在控制任务创建前直接加载到 `s_context.config`，并把 revision 返回给启动日志；`esp_base_protocol_start` 只复制小型元数据，且拒绝未成功加载配置的调用。加载失败仍进入原 pending OTA 本地检查和回滚路径，不新增可能失败的临时堆分配。之后仅 `control_task` 的配置轮询、命令处理、状态报告和网络 owner 配置入口读写该对象；MQTT、FRP、Wi-Fi owner 在各自入口复制所需配置，不让 worker 持有该对象地址。OTA worker 只读取独立的 `s_ota_request`，不读配置。`ebase_config_decode` 在所有校验通过后才改写目标；加载失败不部分覆盖旧配置，写入不确定的分支仍以重新加载成功与否裁决连接。

MQTT owner 的 `configure` 与 `poll` 由唯一 `control_task` 串行调用。`MESSAGE` handler 会同步进入 `handle_line`，其 MQTT `config.set` 在当前调用中被拒绝；配置 revision 刷新发生在下一轮 `poll` 前，不会在 handler 使用事件 payload 期间改写 union。公开 `emqtt_create` 在返回前把配置复制进 runtime；该调用结束后无论结果均按原逻辑擦除临时配置，不保留 union 地址。Host 回归覆盖完整 MESSAGE 消费后立即重新配置。此节省**不包含**公开 MQTT runtime 后续创建的消息槽、重组缓冲和配置副本，也不包含 `s_context.config`、FRP owner 配置与 NVS 提交候选。真实 MQTT、FRP、Wi-Fi 在线动态内存尚未进入下表。

## 同一 QEMU 探针前后对照

仓外复制[五组件 QEMU 容量探针](https://github.com/darren-you/esp-container/blob/77155349795f3b6564e6f6fbbacb61e884e583ad/docs/operations/five-component-qemu-capacity-probe.md)，在不覆盖前次探针的独立副本中只替换本表 Base 源码。探针依赖仍为 Base `31f5ebc` 的普通 MQTT `5bff093`、FRP `3a40a2c`、OTA `3c3f72b` 锁，另外显式加入 Container `00c788e` 和 WAMR `a34d721`；它**未**引入其它仓 `codex/c3-low-memory` 分支。构建使用仓外临时 RSA 测试键、64 KiB guest，以及专为 QEMU 绕过 ADC2 未模拟校准的空实现；镜像不可刷实板。FRP、MQTT 和 OTA 的路径仅被链接，没有真实连接或下载。

| 阶段 | 原 free／最大连续块 | 首批优化 free／最大连续块 | 首轮分支 free／最大连续块 |
| --- | ---: | ---: | ---: |
| Base 初始化前 | 151,088／114,688 | 173,920／114,688 | 185,868／114,688 |
| `ESP_BASE_READY`、guest 已关闭 | 101,716／90,112 | 124,924／114,688 | 136,892／114,688 |
| 64 KiB guest 在 Base READY 后存活并完成事件 | 11,328／7,680 | 34,500／26,624 | 46,504／34,816 |
| guest 关闭且线程回收后 | 101,716／90,112 | 124,924／114,688 | 136,892／114,688 |

首轮分支相对首批优化的 READY free 增加 11,968 字节，guest 存活时增加 12,004 字节；这些数值受动态分配和连续块布局影响，**不能**当成额外静态节省相加。两次 guest 生命周期的 open/init/event/stop 均为 0，guest 返回 3。存活时总 free **46,504** 字节，仍低于 48 KiB 水位 2,648 字节；最大连续块 34,816 字节，也无法容纳 FRP 一次 65,552 字节 AEAD 接收区。

## 首轮链接图的其余 DRAM 占用

首轮探针的 `.dram0.bss` 为 `0x1a158` 字节，`.dram0.data` 为 `0x3578` 字节。该阶段最大的在用对象如下；被移除的 `s_protocol`、`s_event` 不再单列或重复计入：

| 对象 | 字节 | 现有用途 |
| --- | ---: | --- |
| Base `s_reader` | 9,228 | USB 最大 JSON 行接收 |
| Base `command` | 8,400 | 当前解析命令及完整配置 |
| Base MQTT owner `s_work` | 7,716 | 互斥复用 MQTT 创建配置与完整事件输出 |
| Base `s_context` | 7,632 | 控制任务长期上下文与配置 |
| Base `s_fingerprint_bytes`、NVS `s_load_bytes`／`s_commit_bytes` | 各 7,618 | 配置规范编码摘要、读取／读回及提交比较 |
| NVS `s_work`、Base `s_candidate` | 各 7,608 | 事务工作值与 Wi-Fi 候选期配置 |
| Base `s_guard` | 4,872 | 32 槽去重合同 |
| Base FRP owner `s_config` | 2,730 | 网络 worker 生命周期之外的独立配置副本 |

`.dram0.data` 的最大单段是固定 SDK `spi_flash` 的 `4,452` 字节常量；其次是 SDK Wi-Fi／PHY 的 960／844 字节初始化数据。WAMR 在本镜像的 `.bss/.data` 合计仅 **44／28 字节**，Container 为 **1／8 字节**，公开 MQTT runtime 为 **4／0 字节**，公开 FRP 为 **0／0 字节**；它们的主要开销在运行时堆、任务栈与网络会话，不能通过裁剪这些静态符号解决。此首次快照中 `s_fingerprint_bytes` 仍承担配置规范编码摘要，末节记录它的复用；缩短 USB 行、去重槽或消息载荷会改变既有协议容量，本分支没有实施。

FRP 在 `c5fbe40` 的 32 位对象账本给出会话、Yamux、AEAD、client、worker 栈和 TLS 对象的显式并存下界 100,496 字节；该提交相对 Base 所锁 `3a40a2c` 没有 `src/` 或 `include/` 变更。即使 guest 完全关闭，以本次无网络 READY 的 136,892 字节扣除这个下界也仅剩 **36,396 字节**，还未计 FRP CA、Mbed TLS／PSA、lwIP、Wi-Fi、MQTT 会话及网络临时对象。因此这次优化不能证明 FRP 即使与 guest 互斥也能稳定连接，更不能证明两者并发。

## 核对与边界

- `bash firmware/tests/run_host_tests.sh` 的 ASan／UBSan 全套通过；其中 `config_store` 覆盖条件提交与不确定写入，`ota_startup` 覆盖配置加载失败后的 pending OTA 回滚行为，`mqtt_owner` 覆盖完整 MESSAGE 消费后立即重配置、配置副本与 stop 失败保守拒绝。
- 固定 SDK 的普通 ESP32-C3 构建通过，镜像 `0xeaa40` 字节、SHA-256 `2d3b182d84b65c2c24e6b864fed478619e07833a7c4fa42fecbe9af1d47d53f2`；仓外复制的测试键签名五组件 QEMU 镜像 `0x121000` 字节、SHA-256 `d3495ddde08fa4395e90b30045369699d159588692f56d9fdaf75e7e8c7341b7`，启动、Base READY 和两次 64 KiB guest 生命周期通过。QEMU 是无真实射频、Broker、FRPS 或 HTTPS 的容量切片。
- 此分支不改变 4 MiB 分区；两应用槽与业务包槽的正式几何、真实网络峰值、FRP 与 Container 互斥调度、MQTT／OTA 同板运行和实板回滚仍待完整裁决与验证。`_heap_start` 和 QEMU 数值不能替代设备验收。

## 新版精确锁的签名 QEMU 复测

从本分支源码另建仓外探针，只在其副本添加静态链接引用、64 KiB guest 生命周期和 QEMU 不支持的 ADC2 校准空实现。全新解析的七依赖锁 SHA-256 为 `f05c54cb7a1e15361b8a87b1e135ccc01ab3744490c3520de3ba583f05c582e2`：MQTT `ccf81df2215cfddd87aff97afdd2e7f17e50fbaa`、FRP `c56a0f32d96c75fd28e2c04146383348d8ce2829`、OTA `3c3f72b823ce856b02f838fef17db1368e6d5448`、Container `60b65d21e4c1bf4935e791214eb5ff7174563242`，WAMR `a34d721b630213f59fde0b40cebbb980903660e8`，以及既定 cJSON/IDF。`esp_base.map` 确认 `efrp_tls_step`、`esp_mqtt_client_start`、`eota_preflight`、`econtainer_runtime_open`、`wasm_interp_call_wasm` 均进入镜像。

固定 SDK 构建的测试键签名镜像为 **`0x121000` 字节**，SHA-256 `26b153a46d484ba5a0d3683aabdfd7a8b56a5fb35c1255c8f369726846b6c295`；`espsecure verify-signature --version 2 --keyfile` 核对第 0 个 RSA 签名块。`idf.py qemu --qemu-extra-args=-no-reboot` 从合并的 4 MiB 仿真 Flash 启动；日志 SHA-256 为 `4a0752fc7c28cd2c83f87c56c3877c50b98223e8cbf80bd2e67bc60cc82b5373`，观察 28 秒后由宿主终止仿真。两次 guest 都输出 `open=0 init=0 event=0 stop=0 guest=3`，第二次在 `ESP_BASE_READY` 后运行。新锁观测的 Base READY free／最大连续块为 **136,892／114,688 字节**，guest 存活并完成事件时为 **46,504／34,816 字节**，关闭并回收线程后恢复为 **136,892／114,688 字节**，与上表旧锁静态路径数值相同。

MQTT C3 改动只在创建会话后缩小运行实例，QEMU 无凭据、无 Broker 连接，故不会出现其 4,360 字节常态节省；三槽满时还有第四条在途的额外申请。FRP、MQTT、OTA 在探针中只强制链接，没有建立会话或下载。guest 存活的最大连续块 **34,816 字节**仍不足以申请 FRP 单次 **65,552 字节** AEAD 接收区；而 QEMU 没有真实射频、TLS 或 Flash 包槽负载。这份复测确认新锁可签名链接并在仿真中执行单页 guest，不能证明五能力并发或实板容量；ADC2 空实现使该镜像绝不可用于物理板。

## 第二轮配置缓冲生命周期复用

从上述新锁探针再复制独立工程，只叠加本分支的 Base 配置缓冲改动；七依赖锁仍为 `f05c54cb7a1e15361b8a87b1e135ccc01ab3744490c3520de3ba583f05c582e2`。`config.set` 的规范编码不再占用独立的 `s_fingerprint_bytes` 7,618 字节：唯一控制任务在命令处理期间借用 NVS `s_load_bytes`，同步完成原有 `SHA-256("config.set" || canonical_blob)`，返回前擦除整个编码缓冲。该阶段没有 NVS 加载或提交；USB `s_reader` 仍保留可能只收到一部分的行，不能与 MQTT 回调中的指纹计算复用。

配置候选期仍由 `s_candidate` 保存完整配置。候选连接证实后，`poll_configuration` 才使用已结束的命令解析空间 `command.config` 作为 NVS `work`，加载旧 revision、构造新 revision、写入、读回，并在返回前完整擦除它。`s_commit_bytes` 和 `s_load_bytes` 仍同时保存提交编码与读回编码并逐字节比较；写入后任何失败继续返回不确定，原配置及恢复决策保持原路径。没有缩短 9,216 字节 USB 行、7,618 字节配置 blob 或 32 个去重槽，也没有把这些静态对象移到 6 KiB 控制任务栈或改为运行时堆申请。

同一签名五组件链接图中，`s_fingerprint_bytes` 与 NVS `s_work` 消失；其原大小分别为 7,618 和 7,608 字节，对齐后的 `.dram0.bss` 从 `0x1a158` 降至 `0x165d8`，减少 **15,232 字节**，`.dram0.data` 保持 `0x3578`。`_heap_start` 从 `0x3fcad8e0` 前移至 `0x3fca9d60`，相对起始 `0x3fcb6100` 累计释放 **50,080 字节**初始 8-bit 堆空间。常驻的 `s_reader`、`command`、`s_candidate` 和 `s_guard` 分别仍为 9,228、8,400、7,608 和 4,872 字节；NVS 加载与提交编码缓冲各保留 7,618 字节。它们的并存时间或协议容量要求不支持继续直接重叠。

| 同一新锁签名 QEMU 阶段 | 第二轮前 free／最大连续块 | 第二轮后 free／最大连续块 |
| --- | ---: | ---: |
| Base 初始化前 | 185,868／114,688 | 201,100／114,688 |
| `ESP_BASE_READY`、guest 已关闭 | 136,892／114,688 | **152,124／114,688** |
| 64 KiB guest 在 Base READY 后存活并完成事件 | 46,504／34,816 | **61,736／40,960** |
| guest 关闭且线程回收后 | 136,892／114,688 | **152,124／114,688** |

`bash firmware/tests/run_host_tests.sh` 的 ASan／UBSan 全套通过；`config_store` 用最大 7,618 字节配置验证复用编码与直接编码逐字节相同，覆盖消费失败及原有 NVS 写前、写后、commit、读回故障，并检查工作配置被全部擦除。固定 SDK 普通 C3 构建通过，镜像 `0xeac20` 即 **961,568 字节**、SHA-256 `5dff4ee274ac12afa1bd1a4fbc3a3056a9f16ad0faa4ce8fccf7ba3da555027f`。仓外临时测试键的签名五组件镜像仍为 `0x121000`，SHA-256 `72ca1c080823b67531207abd1a7ce19e5628e53edb99bed6a7186491440ce813`，RSA 签名块 0 验证通过；QEMU 日志 SHA-256 `50083cc7a2f0a9527053d95f044d22b46378bb977a05e48060c5642da574d011`。两次 guest 生命周期都输出 `open=0 init=0 event=0 stop=0 guest=3`，无崩溃；28 秒观察后由宿主终止仿真。

这份 QEMU 切片的 guest 存活 free 超过 48 KiB 水位，但最大连续块 **40,960 字节**仍小于 FRP 单次 **65,552 字节** AEAD 接收申请。即使 guest 关闭，152,124 字节 free 减去 FRP 已知 100,496 字节并存下界仅余 51,628 字节，尚未计入实际 Wi-Fi、TLS、Broker、FRPS、OTA 与网络临时负载。QEMU 专用 ADC2 空实现、无真实射频和临时测试键使该镜像不可用于物理板；本结果不证明五能力并发、真实联网容量、包槽几何或实板回滚。
