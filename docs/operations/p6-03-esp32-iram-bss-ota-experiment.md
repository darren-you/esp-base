# P6-03：ESP32 静态工作区 IRAM BSS 与 OTA 并发实验

2026-09-28。承接[六静态 RX 缓冲签名检查点](p6-03-esp32-rx6-ota-capacity-checkpoint.md)，本实验阶段**仅在仓外 QEMU 源码副本**使用固定 SDK `esp_attr.h` 的 `IRAM_BSS_ATTR`，把经典 ESP32 的 Base 协议静态工作区放到可字节访问 IRAM。此属性在不支持该能力的目标上为空；本实验阶段尚未修改正式 Base 产品源码及两个受管组件锁。每档均重新完整构建测试键 ECDSA v1 app，官方验签与 `0x120000` 槽尺寸检查通过，按 app 完整摘要重建 ECS2 sequence 6 和全新 4 MiB Flash。ESP32 既有单核／8BIT IRAM 与 TLS 大缓冲优先 IRAM 配置保持不变。后续产品源码落地与第三预备流检查另见[产品静态 IRAM 检查点](p6-03-esp32-static-iram-product-checkpoint.md)。

| 仓外签名 QEMU 变体 | 新放入 IRAM BSS 的 Base 对象 | 合计静态字节 | app SHA-256 | 本次成功负载的普通内部 8BIT 堆历史最低 | FRP 工作完成时 IRAM free／最大连续块 |
| --- | --- | ---: | --- | ---: | ---: |
| [六 RX 原样基线](p6-03-esp32-rx6-ota-capacity-checkpoint.md) | 无 | 0 | `cbc77b4fceee3e0fa6032afd38312258e39644cb7626ae57ad684b7996a952dc` | 30,052 B | 38,200／27,648 B |
| `mac-work-1:/private/tmp/esp-base-ota-iram-bss-capacity-20260928/` | 协议上下文 7,636 B、MQTT 入站事件 4,388 B | **12,024 B** | `35f636ae29efc68e61f735fa5f6a916080d3c3c7f24b80385fc14c6973e53d32` | **43,876 B**，低于门 5,276 B | 21,564／17,408 B |
| `mac-work-1:/private/tmp/esp-base-ota-iram-bss-more-capacity-20260928/` | 上项加请求 guard 2,568 B、结果槽 2,048 B、FRP 状态重放槽 768 B | **17,408 B** | `d28ddc6573502ebd0d3242a7368582ff5b87c7b2934f9dd5da6b2731053f6cfe` | **50,708 B**，高于门 **1,556 B** | 20,788／**12,800 B** |

两档成功轮次均由真实签名 ABI 2 guest 到 `RUNNING`、Base 到 `READY`；官方 FRPS TLS `verify=0`、Pong 3，两条并行工作流各方向 **300001 B** 逐字节回显且 `completed=2 failed=0`；三条各 **4096 B** QoS1 MQTT 消息交付。公开 `eota_preflight`／`eota_prepare` 经严格 HTTPS 收到独立签名 Base app **1,114,100 B**，目标 inactive app 读回与源逐字节相同，owner 释放；运行 app_0、产品包区、`base_store`、bootloader 和分区表保持原字节，scratch 复原全 `0xff`。两档成功 UART SHA-256 分别为 `cb326a42bbed7a9e15f5a03e8ca1f88cadca734fdcecfcdfc97561d7d3b60e02`／`a53dd6d33133bfdfab09baee64684d9399a58e930b028f8cb88a51ff4b32225b`，种子 Flash SHA-256 分别为 `07a1206f76570725bd2e17a490794903d6ddb268281e47e5b7dd45cb53a777ab`／`32d78ff303b22c3f7b1c749b36434f700a7a9678161825b9673ee40adb14cada`。各档源文件、完整构建、验签、日志与 Flash 均留在对应仓外目录。签名镜像与时序不同，表中低水差额不是逐对象精确节省量。

**失败原样保留。** 两档各自首次运行都在 OTA `esp_ota_begin` 擦除 Flash、OpenETH 收包时触发 QEMU `Cache error` 并复位；失败 UART SHA-256 分别为 `9fe0842bedf4e6ce0a46d71b40f7ccfcd8a3f9d4a6557295928df342f3b90dd8`、`95afce0b51e5d2682f091656a11c37a9a070abb7e9a5c347555e974106db3f4b`。固定 ELF 回溯落在 SDK `emac_opencores_isr_handler` 的 UART 日志路径，Flash cache 当时关闭；相同镜像、重新播种的后续独立轮次完成全负载。该时序故障不能归为 IRAM BSS 对象访问，但也不能忽略为稳定通过。没有修改 SDK、OpenETH ISR 或产品 OTA 逻辑来绕过它。

扩展档的 **50,708 B** 只跨过当前双流／三消息／`eota_prepare` 的一个 QEMU 历史低水门，**不是 P6-03 验收**。扩展档在工作完成时 IRAM 最大连续块仅 **12,800 B**，本实验阶段尚未证明两活跃加一预备 FRP 工作流、断线重连时的严格 IRAM 会话申请和 16 KiB TLS 收包分配；第四条 MQTT 在途、最大 64 KiB FRP 认证记录、正式 Base `ota.start` 收据／Container stage／选槽、实体 Wi-Fi 与掉电恢复也均未覆盖。仓外属性实验至此没有并入产品源码；后续第三预备流与双目标构建验证见上述产品检查点。现有两块 4 MiB 板和 **49,152 B** 门保持不变。
