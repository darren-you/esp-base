# ESP32 FRP scratch 候选几何的签名 QEMU 检查点

2026-09-27，从 `esp-base@7aea9dc1344dff388429f4a72c378fe49adc264f` 独立归档，只在 `mac-work-1` 的隔离构建副本使用 `frp_scratch@0x3ea000/0x10000` 和 `base_store@0x3fa000/0x6000`，复验真实签名 guest 与 FRP scratch 启动恢复同存。**这是一份 P6-03 布局输入，不是正式 ESP32 分区决定，也没有向实体设备写入。**

## 输入和构建

原始目录为 `mac-work-1:/private/tmp/esp-base-classic-deadline-scratch-qemu-20260927/`，下文记为 `$RUN`。Git 归档 gzip SHA-256 `521592f8139dd89b143705786c1fd52dd93bfabe61a42c1f92f9bf3532b87c3e`；固定 SDK 是 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 和 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。本次主固件锁精确消费 Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、OTA `f4fb0b4f3fa7b384edf540bac626314418156d22`、FRP `98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`。

仓外候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`，保留双 `0x120000` app、三 `0x82000` 包槽及 `at_old_raw@0x3e6000/0x4000`，在其后放置独立 64 KiB FRP scratch 和六页 Base NVS。仓内**正式 ESP32 CSV** SHA-256 仍为 `f3f29e52f2ed0ccb3fbb3faf9e3d978d359aa9a958c2b3a6120399af70f11b73`，未修改。仓外签名 sdkconfig SHA-256 `ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`，显式启用 FRP scratch 并绑定其 label／offset，Container NVS 绑定候选的六页区域；使用既有仓外 ECDSA v1 测试键。

普通 Base 没有产品卸载命令消费者。归档副本复用[测试变体准备脚本](../../tools/prepare_qemu_product_uninstall_probe.py)，只在 Base `READY` 后创建 16 KiB FreeRTOS 任务，持唯一 storage claim 调用正式 `stop_confirmed → product_uninstall → product_boot → release`。原 `esp_base_main.c` SHA-256 `41523e4ba841fcf5487b414b5fb9d574c1b18823e28e956fe5ce383e7e921a3b`，测试变体为 `d426b9b6a14c451763fddd04af0d6964359e8323974deef67962b839bfb31049`；没有修改 FRP、Container、WAMR、NVS 或 OTA 实现。签名 app 为 `0x10fff4` B、SHA-256 `fbda43fe2e9899e7bcb5e0485779717ff249757319f13e75e51a03380033891f`，双 app 槽各余 `0x1000c`；官方 ECDSA v1 应用与分区表验签、官方尺寸门通过。`compile_commands.json` 确认 WAMR `wasm_interp_classic.c`、Container `runtime.c` 均真实编入 `WASM_ENABLE_CLASSIC_WALL_CLOCK_LIMIT=1`，FRP `idf_flash_store.c` 也进入目标构建。

签名 ABI 2 counter 包为 10,240 B，SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`。`$RUN/esp32/seed.c` 用本轮锁的 Container slots／包校验／Wasm 校验源码，对**本轮新签名 app**生成 `CONFIRMED` 的 ECS2 sequence 6 和包区；这是离线前置状态制造，不是 Base 设备安装 API。固定 SDK 的 NVS V2 generator 把 blob 放入六页 `base_store`。合成 Flash 的 FRP scratch 预置 `byte[i] = (0x5a + 17i) & 0xff`，整个 64 KiB 的 SHA-256 为 `11006f8d077c72f1c185ef37f4dc217dce5433af4ab129ba2ef4a05081bd8444`；其中 65,280 B 非 `0xff`，用来观察启动擦除。`$RUN/esp32/assemble_flash.py` 按本轮 `flasher_args.json` 写 bootloader、签名分区表、otadata、签名 app，再写包区、scratch、NVS；初始完整 4 MiB Flash SHA-256 `4b1443d58ed98daab3a64e005ec031c2dff9ba9919eb5be5da2c281076bc6f49`。逐段摘要在 `$RUN/esp32/flash-receipt.json`。

## QEMU 运行和读回

Espressif QEMU `9.2.2 (esp_develop_9.2.2_20260417)` 使用合成 Flash 的 MTD 驱动，未连接实体板。首次运行 UART 显示 `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、Base `READY container=running`，然后 `QEMU_PROBE_STOP result=1`、`UNINSTALL result=0 (COMPLETE)`、同 boot `SAME_BOOT result=2 (EMPTY)`、`DONE ... released=1`。同片 Flash 冷启动显示 Base `READY container=empty`。原始 `$RUN/esp32/scheduled-uart.log` SHA-256 `f80dc8c33f5629aca8bff8f05a1bae5e00653083e0964830b8e6020c1f67be54`；`reboot-uart.log` SHA-256 `c3a8d3b629c3272a3398a6a3efa875a81d03c2442a5603d0febfa91b2c055203`。

启用的正式 Base 启动路径在任何 pending OTA 确认前绑定精确 FRP 分区并调用 `efrp_aead_flash_store_recover`；该受管 provider 的 recover 通过唯一 storage owner 擦除整个 64 KiB scratch，失败则在 Base 启动处阻断。首次运行后的 scratch **全部为 `0xff`**，SHA-256 `71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063`；冷启动仍全部为 `0xff`。因此本轮不只是空 scratch 的启动通过，也观察到真实受管恢复擦除；它没有运行 FRPS 会话或密文读写。

官方 NVS parser 对初始、卸载后和冷启动的六页区域均报告活动页 CRC 正常，活动 ECS2 sequence **6 → 7 → 7**，最终 blob SHA-256 `ad8adf4369ea4d7755b5dc7e168883d7ba4a0e9d8ff3e1ee00514924fbb8b97e`。首次运行相对初始 Flash 只改变系统 NVS **101 B**、otadata **12 B**、scratch **65,280 B**（预置非空字节全部擦除）及 Base NVS **353 B**；两 app 槽、整个包区和 `at_old_raw` 逐字节不变，其他区域变化为 0。冷启动后的完整 Flash 与卸载后逐字节相同，SHA-256 均为 `962c5ed606023fbea9b31cd472516d1dbdcb6a6a784e9297cfe6a748bba9be90`。解析脚本与逐区原始收据为 `$RUN/esp32/analyze_flash.py`、`region-diff.json`（后者 SHA-256 `150fda3f6b14644dd1f7f9d55c906485797cab031f3746652e8fb0f61145994d`）。

本证据只支持这份**候选几何**上的测试签名 guest 启动、FRP scratch 启动擦除、产品卸载和同片冷启空绑定。没有 FRPS session、大密文记录、OTA／FRP／guest 并发、实体 Flash 时延、旧 AT 或 C3 数据迁移、掉电原子性及实板六页 NVS 长期容量证据；counter guest 也没有故意耗尽墙钟期限。正式镜像仍无设备产品卸载命令、安装 API 和结果收据，P6-03/P7-02 不据此完成或冻结布局。
