# ESP32-C3 小内存 Base 配置缓冲复测

本记录对应独立 `codex/c3-low-memory` 分支，以 `esp-base@31f5ebcc0bbc756fe5e78cb7c53f9042832ce286` 为起点。固定 SDK 为公开 `esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c` 和 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d`。本次只调整 Base 配置数据的内存存放，不修改配置 schema、NVS 事务、分区、TLS、FRP、MQTT 或 OTA 协议，也不刷写设备。

## 消除的长期占用

用当前五组件 ESP32-C3 链接探针的 `esp_base.map` 核对以下三个互不重叠的静态对象：

| 对象 | 原占用 | 调整后的唯一数据路径 |
| --- | ---: | --- |
| 启动入口 `s_config` | 7,608 字节 | NVS 直接读入启动上下文 `s_protocol.config`，再由协议组件按既有接口复制到其长期上下文 |
| 协议控制 `s_committed` | 7,608 字节 | 条件提交成功时直接写入 `s_context.config`；提交失败保持旧值，写入结果不确定时按原逻辑重读持久配置并决定连接状态 |
| NVS `s_readback_bytes` | 7,618 字节 | 提交后读回调用结束并擦除 `s_load_bytes`，再在该缓冲编码读回值，与独立的 `s_commit_bytes` 做逐字节比较，最后擦除两者 |

原始对象合计 22,834 字节；链接对齐后的 `_heap_start` 从 `0x3fcb6100` 前移至 `0x3fcb07d0`，**准确释放 22,832 字节的初始 8-bit 堆空间**。对照镜像的 `.dram0.bss` 从 `0x22980` 变为 `0x1d048`；签名五组件镜像仍为 `0x121000` 字节。NVS 加载和提交继续由启动线程、随后唯一控制任务串行执行，提交候选与读回值始终位于两个独立缓冲；失败时不擦除持久配置，也不把未知结果报为成功。

`s_context.config` 的启动复制发生在控制任务创建前。之后仅 `control_task` 的配置轮询、命令处理、状态报告和网络 owner 配置入口读写该对象；MQTT、FRP、Wi-Fi owner 在各自入口复制所需配置，不让 worker 持有该对象地址。OTA worker 只读取独立的 `s_ota_request`，不读配置。因此条件提交向 `s_context.config` 的 7,608 字节写入没有另一任务同时读取。`ebase_config_decode` 在所有校验通过后才改写目标；加载失败不部分覆盖旧配置，写入不确定的分支仍以重新加载成功与否裁决连接。

这些节省**不包含** Base MQTT owner 的静态 `s_emqtt_config`（7,716 字节）和 `s_event`（4,388 字节），也不包含公开 MQTT runtime 后续创建的三个 4,368 字节消息槽、4,376 字节重组缓冲与 7,716 字节配置副本。`s_protocol.config`、`s_context.config`、FRP owner 自己的配置和 NVS 提交候选依然存在，不能把它们再算成这次释放。MQTT、FRP 和 Wi-Fi 的实际在线动态内存尚未进入下表。

## 同一 QEMU 探针前后对照

仓外复制[五组件 QEMU 容量探针](https://github.com/darren-you/esp-container/blob/77155349795f3b6564e6f6fbbacb61e884e583ad/docs/operations/five-component-qemu-capacity-probe.md)，只替换上表三处 Base 源码。探针依赖仍为 Base `31f5ebc` 的普通 MQTT `5bff093`、FRP `3a40a2c`、OTA `3c3f72b` 锁，另外显式加入 Container `00c788e` 和 WAMR `a34d721`；它**未**引入其它仓 `codex/c3-low-memory` 分支。构建使用仓外临时 RSA 测试键、64 KiB guest，以及专为 QEMU 绕过 ADC2 未模拟校准的空实现；镜像不可刷实板。FRP、MQTT 和 OTA 的路径仅被链接，没有真实连接或下载。

| 阶段 | 原 free | 本分支 free | 原最大连续块 | 本分支最大连续块 |
| --- | ---: | ---: | ---: | ---: |
| Base 初始化前 | 151,088 | 173,920 | 114,688 | 114,688 |
| `ESP_BASE_READY`、guest 已关闭 | 101,716 | 124,924 | 90,112 | 114,688 |
| 64 KiB guest 在 Base READY 后存活并完成事件 | 11,328 | 34,500 | 7,680 | 26,624 |
| guest 关闭且线程回收后 | 101,716 | 124,924 | 90,112 | 114,688 |

初始 free 差值恰为 22,832 字节，与 map 一致；READY free 增加 23,208 字节、guest 存活时增加 23,172 字节。后两者受本轮动态分配与连续块布局影响，**不能**当成额外静态节省相加。guest 存活时总 free 34,500 字节，低于原计划 48 KiB 水位；最大连续块 26,624 字节，也无法容纳当前 FRP 一次 65,552 字节 AEAD 接收区。

## 当前链接图的其余 DRAM 占用

同一优化后探针的 `.dram0.bss` 为 `0x1d048` 字节，`.dram0.data` 为 `0x3578` 字节。按 linker map 中输入段及源文件归类，不计填充的主要 `.bss` 来源为 Base `device_protocol` 67,805 字节、`remote_config` 22,844 字节、Base `main` 7,709 字节、SDK Wi-Fi 9,331 字节；这些来源与上表三个已移除对象不能重复相加。最大的仍在用对象如下：

| 对象 | 字节 | 现有用途 |
| --- | ---: | --- |
| Base `s_reader` | 9,228 | USB 最大 JSON 行接收 |
| Base `command` | 8,400 | 当前解析命令及完整配置 |
| Base MQTT owner `s_emqtt_config` | 7,716 | 创建 MQTT runtime 前的配置装配 |
| Base `s_protocol`／`s_context` | 各 7,632 | 启动参数与控制任务长期上下文 |
| Base `s_fingerprint_bytes`、NVS `s_load_bytes`／`s_commit_bytes` | 各 7,618 | 配置规范编码摘要、读取／读回及提交比较 |
| NVS `s_work`、Base `s_candidate` | 各 7,608 | 事务工作值与 Wi-Fi 候选期配置 |
| Base `s_guard`、MQTT owner `s_event` | 4,872／4,388 | 32 槽去重合同与完整消息事件 |
| Base FRP owner `s_config` | 2,730 | 网络 worker 生命周期之外的独立配置副本 |

`.dram0.data` 的最大单段是固定 SDK `spi_flash` 的 `4,452` 字节常量；其次是 SDK Wi-Fi／PHY 的 960／844 字节初始化数据。WAMR 在本镜像的 `.bss/.data` 合计仅 **44／28 字节**，Container 为 **1／8 字节**，公开 MQTT runtime 为 **4／0 字节**，公开 FRP 为 **0／0 字节**；它们的主要开销在运行时堆、任务栈与网络会话，不能通过裁剪这些静态符号解决。`s_protocol`、`s_fingerprint_bytes` 或 MQTT 临时配置是下一步可研究的整块对象，但分别需要重新裁决上下文所有权、保持规范编码摘要完全一致、验证重配置瞬时堆峰值；仅从 map 不能证明可以安全释放。缩短 USB 行、去重槽或消息载荷则改变既有协议容量，本分支没有实施。

FRP 在 `c5fbe40` 的 32 位对象账本给出会话、Yamux、AEAD、client、worker 栈和 TLS 对象的显式并存下界 100,496 字节；该提交相对 Base 所锁 `3a40a2c` 没有 `src/` 或 `include/` 变更。即使 guest 完全关闭，以本次无网络 READY 的 124,924 字节扣除这个下界也仅剩 **24,428 字节**，还未计 FRP CA、Mbed TLS／PSA、lwIP、Wi-Fi、MQTT 会话及网络临时对象。因此这次优化不能证明 FRP 即使与 guest 互斥也能稳定连接，更不能证明两者并发。

## 核对与边界

- `bash firmware/tests/run_host_tests.sh` 的 ASan／UBSan 全套通过；其中 `config_store` 覆盖条件提交、冲突、写前失败、写后不确定、读回不一致与旧配置保留，`ota_startup` 覆盖配置加载失败后的 pending OTA 回滚行为，命令解码检查仍验证 v3 序列化。
- 固定 SDK 的普通 ESP32-C3 构建通过，镜像 `0xeaa00` 字节、SHA-256 `51d136510b43332ebfba1b9806ee57a1f0d4288bce37961d2918874b2184f5d5`；仓外复制的签名五组件 QEMU 探针构建、启动、Base READY 和两次 64 KiB guest 生命周期通过。QEMU 是无真实射频、Broker、FRPS 或 HTTPS 的容量切片。
- 此分支不改变 4 MiB 分区；两应用槽与业务包槽的正式几何、真实网络峰值、FRP 与 Container 互斥调度、MQTT／OTA 同板运行和实板回滚仍待完整裁决与验证。`_heap_start` 和 QEMU 数值不能替代设备验收。
