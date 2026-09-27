# ESP32 签名 guest 与 FRP reader 资源归因

2026-09-27，P6-03 只读归因检查点。输入为 Base `0babeec8f39f8af528e23ffa97e94bc77ecd2f79` 的[认证记录 QEMU 检查点](esp32-frp-authenticated-record-qemu-checkpoint.md)：固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`。以下运行全部使用仓外合成 4 MiB Flash、测试签名键和 Xtensa QEMU；**没有修改正式分区或实体设备**。

## 同源运行读数

原探针镜像 `1e5dede83833850fa4f7c15d2aeda6c213add084cabd6758fc996094ecc81ee2` 的合成 Flash SHA-256 为 `5fea5c2ce91e9f5581a5b4b2f6cffa8c70a7b7fc17a3c797a2403bcc88b481ee`。在 `mac-work-1:/private/tmp/esp-base-p603-resource-ab-20260927/` 以这份**完全相同**的签名镜像和初始 Flash 另起 QEMU，等待 `QEMU_FRP_PASS` 后再观测 11 秒。UART SHA-256 `33acbb3e32805aef7817e6ce7ade0f67bfc94fdb20d0f79bd6e58fefc607d254`。探针任务退出后，Base 连续两次上报当前 free **46,776 B**；整次启动的 minimum 仍为 **33,164 B**。退出回收不能改写已经发生的低水位。

对照组在仓外复制上述 Base 源码归档，**只去掉 QEMU 认证探针**；产品策略、FRP scratch、正式 Base/Container 调用链、依赖锁和 `sdkconfig` 保持相同。两份 `sdkconfig` 的 SHA-256 均为 `ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`。重新生成的 ECDSA v1 签名 app 仍为 `0x10fff4` B，SHA-256 `7f17f037a662bfd7e95b1f88c63258de4e4ba7785719b64cedc9b8dd24b0b124`，官方 `espsecure verify-signature --version 1` 返回有效；按**新 app 完整摘要**重新生成 ECS2 sequence 6 的同一真实签名 ABI 2 counter 包绑定。合成 Flash SHA-256 `01e81601768a2c0a5b625c72824d7ad973c9f21a962bc917e42809286f2fd0ac`。QEMU 在 `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0` 后到达 Base `READY container=running`；运行 UART SHA-256 `8922eb43e109bce1c15fef36637f22fcc22360e8910687ead7644ef1e7533777`，原始构建、验签、seed 和 UART 位于 `mac-work-1:/private/tmp/esp-base-p603-resource-noprobe-20260927/`。

| 同阶段空闲堆 | 原认证探针镜像 | 无探针对照镜像 | 可证明的差异 |
| --- | ---: | ---: | ---: |
| guest 启动前，稳定上报 | 119,976 B | 125,096 B | 探针静态缓冲占 5,120 B |
| guest 创建期间的首次低水后，当前值 | 42,304 B | 47,424 B | 两边相对前行均降低 77,672 B |
| guest `RUNNING` 后、认证任务启动时 | 34,128 B | 无任务 | 原镜像相对 42,304 B 再少 8,176 B |
| 任务退出后或无探针稳定运行，当前值 | 46,776 B | 51,896 B | 两边相差 5,120 B |
| 启动全程 minimum | **33,164 B** | **43,636 B** | 两者都未达到 49,152 B |

Base 上报当前 free 使用 `esp_get_free_heap_size`，minimum 使用 `MALLOC_CAP_DEFAULT`；探针还直接用 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 取样，本次无 PSRAM 的 ESP32 QEMU 两路最低数值一致。原镜像 `.bss` 的 `s_qemu_frp_window` 为 4,096 B、`s_qemu_frp_zero` 为 1,024 B；运行时前两行的精确 5,120 B 差异与它们一致。`qemu_frp_auth` 是额外的 8 KiB 测试任务，**不是正式 FRP session**。原探针在 tag 前不交付明文，认证后 16 个窗口、坏 tag 拒绝和 17 次全长复验均通过；其 reader/provider 不申请第二份 64 KiB 堆缓冲。探针结束时的较高 free 不是启动最低 free；原 QEMU 的 largest 最低 26,624 B、测试任务栈高水余量 4,700 B 仅属于该探针。无探针对照没有独立 largest 或产品 pthread 栈高水采样。

## 最大项与源码边界

Container `src/runtime.c` 的 `wasm_runtime_instantiate_ex` 将额外 host heap 设为零，并为一次执行创建一个 `exec_env`。固定 WAMR 的 Classic 一页线性内存经 `core/iwasm/common/wasm_memory.c`、`core/shared/platform/esp-idf/espidf_memmap.c` 在内部 8BIT 堆申请 **65,536 B**；这是已签名 ABI 2 一页配置所需的最大可证实单项，不是两份常驻 Wasm。Base 产品线程按当前仓外策略另有 16 KiB 原生 pthread 栈，Container `exec_env` 使用 8 KiB 解释器栈，两者与 guest 线性内存承担不同职责。Container 仅复制必须可写的 Wasm code/data 节；Flash 映射在 open 后解除，未发现整包或第二份 64 KiB 常驻副本。上述源码项与两组运行均为 77,672 B 的 guest 启动降幅一致，但该降幅还包括线程、解释器栈、模块元数据及期间释放，不能把全部 77,672 B 都记成线性内存。

FRP reader 使用调用方给出的唯一窗口；IDF Flash provider 的回读临时区在调用栈上，Flash I/O 仅短借 Base storage owner。历史认证握手独立 4 KiB 重复堆缓冲已在本次固定 FRP 锁之前删除，其收益不能再计算一遍。当前真正的 FRP client/session、TLS、Yamux、MQTT 和 OTA 网络负载均未启动，也没有可证明无效的第二份记录堆分配。

Base 产品上下文常驻的验包 workspace 为 4,992＋512＝5,504 B，另有约 136 B 的 `verified_info` 对象被 Container open 内部局部对象覆盖。把 workspace 移到产品线程栈在生命周期上看似可行，但 16 KiB 栈还嵌套 RSA 验签、Wasm 扫描和 WAMR 装载；现有 host 回归使用 32 KiB 产品栈，本轮没有目标 16 KiB pthread 的栈高水证明。即使理想地把全部 5,504 B 还给堆，无探针对照的 43,636 B minimum 也只达到 49,140 B，仍比 48 KiB 门少 12 B，且没有给真实 FRP session 留余量。因此本轮**不改生命周期源码或资源限额**，不以 BSS 理论节省冒充运行态修复。

计划 §13.3 的初始 free heap 门为至少 48 KiB，即 49,152 B。原探针低于门 **15,988 B**；去除整个探针后真实签名 guest 的启动 minimum 仍低于门 **5,516 B**。稳定运行时 free 达 51,896 B 不能覆盖此前低水。最大必要项已归因，尚无已证实且足以恢复资源门的最小无损源码修正；下一步须在代表性真实 FRP/TLS、MQTT、OTA 与 guest 同存下测完整动态峰值，并测产品 pthread 栈高水后才可能裁决工作区复用。P6-03 的完整组合和实体设备验收保持开放。
