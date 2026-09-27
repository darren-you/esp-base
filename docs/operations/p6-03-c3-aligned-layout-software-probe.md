# P6-03 C3 四兆分区对齐与签名启动软件检查点

2026-09-28。维护者已确认继续使用现有 ESP32-C3／ESP32-D0WD-V3 两块 **4 MiB** 板，普通内部 8BIT 堆历史最低门保持 **49,152 B**。本检查点只在 `mac-work-1:/private/tmp/esp-base-c3-aligned-layout-probe-20260928/` 使用固定 ESP-IDF `578cf89c`、当前 Base 源码与七依赖锁（C3 `dependencies.lock` SHA-256 `e5b0c66f402ee518931be19e110fed90df8cbe9049b7d60e64f510fcf8845b54`）、仓外测试签名键和真实 ABI 2 counter 包构造软件候选。实体板、生产凭据及正式仓内分区表均未修改。

## 官方对齐约束与修正后的几何

先尝试双 `0x118000` app 紧贴排列：`ota_1@0x138000`。固定 SDK 的官方 `gen_esp32part.py` 在实际构建第 5 步拒绝 `Partition ota_1 invalid: Offset 0x138000 is not aligned to 0x10000`。因此两槽之间保留 **`0x8000` B 对齐空隙**，不能把未经官方分区生成器接受的算术排布当作交付方案。

| 分区 | Offset | Size | 终点 |
| --- | ---: | ---: | ---: |
| `ota_0` | `0x20000` | `0x118000` | `0x138000` |
| 对齐空隙 | `0x138000` | `0x8000` | `0x140000` |
| `ota_1` | `0x140000` | `0x118000` | `0x258000` |
| `product_pkgs`：三份 `0x82000` | `0x258000` | `0x186000` | `0x3de000` |
| `frp_scratch` | `0x3de000` | `0x10000` | `0x3ee000` |
| `base_store`：18 页 | `0x3ee000` | `0x12000` | `0x400000` |

三个包槽起点分别为 `0x258000`、`0x2da000`、`0x35c000`。旧启动分区和系统 NVS／otadata／phy_init／coredump 保持当前预置值，app 从 `0x20000` 开始；整张候选表使用 data/undefined 的产品包和 scratch、data/nvs 的 Base 存储。仓外 CSV SHA-256 `12baa31c480d76cc2172cac621e233b84828052fcf1b866ae6072212a7230f27`。仓外 C3 OTA policy 同时精确指向 `ota_0@0x20000`、`ota_1@0x140000` 与 `0x118000` 槽，未将它写回正式产品。

## 验签、运行与读回

固定 SDK 完整签名构建通过；RSA v2 测试签名 app 为 **`0x111000` B**，SHA-256 `d75dd8819ba67bb06f309e76e27d274004545c2ea70cb0dde6e48ffe908662fc`，官方 `espsecure verify-signature --version 2` 与 `check_sizes.py` 通过，两 app 槽各余 `0x7000` B。官方分区二进制解码与候选 CSV 全条目一致，分区表 SHA-256 `1a34ef3c84047f4df97bda6bd57b278f3a68d133bef2b2e3d38f0a46f750c909`；最终 `sdkconfig` SHA-256 `ced6ce74e2e99cf808a2e5425408e56e1a131e393253b9692e5ed0b1660a79ab`。

用当前 app 摘要与同一测试包重新生成 ECS2 sequence 6、18 页 Base NVS 和全片合成 Flash；种子 SHA-256 `91ce9ed2e89d40299f25d4a647d6fa4502c0fc876a6cc40807ba15e24c032447`。C3 QEMU 仅在未模拟的 ADC2 校准入口由 GDB 跳过一次，签名 app 字节不变；实际函数返回 `ESP_BASE_CONTAINER_RUNNING=3`，随后到达 Base `READY`。该**无网络**启动点的普通内部 8BIT 堆 `free/largest/min=122,512/114,688/115,148 B`。原始 GDB SHA-256 `64a0f228abf5b55353802957d0533c32df6576f822bb93e486b33215d43c79da`，整片读回 SHA-256 `94cfbe10f24762984752989358ec65d086607c1f94a86a82641f3849c185b747`；bootloader、分区表、双 app、三包区与 `base_store` 均逐字节不变，scratch 在启动恢复时改变 65,292 B。合成 Flash、命令、GDB、官方验签与逐区读回都保存在上述仓外目录。


同一份当前 Base 源码和 ESP32 精确组件锁也完成了 ESP32-D0WD-V3 目标的独立 ECDSA v1 测试签名构建。该目标仍使用既有 ESP32 分区表：签名 app 为 `0x10fff4` B，SHA-256 `158c2d6d78e5907359ed4deee572fbe2e49e4bcd021c4095d0f9e3b97f410d25`；官方 `espsecure verify-signature --version 1 --keyfile` 验签通过，`check_sizes.py` 确认最小 `0x120000` app 槽余 `0x1000c` B。分区表二进制 SHA-256 `0b22156f31a15128763fae584490a1100f1b0675c0e3f55ec159a62eb73e62c5`，生成配置 SHA-256 `8d409908ea1873c3b9e2eaa92400a42bf5e6f525520283fdfffe76c4acef6f3d`；构建日志在同一仓外目录的 `build-esp32.log`。此构建只验证 ESP32 目标当前代码可签名装槽，不是 C3 候选表的跨目标复用，也没有 ESP32 联网运行堆读数。

此前[固定 SDK 的 C3 八页 NVS 容量实验](c3-eight-page-nvs-capacity.md)已经在合成正常路径完成 100 代最大 Base 配置 CAS、OTA／Container blob 共同写入及跨进程重启；本候选的 18 页高于该实验容量，但本次没有在新几何上重跑 100 代、旧数据转换或掉电。现有实体 C3 的 `base_store@0x3e0000/0x20000` 与新 scratch、NVS 均重叠，且已有异常 NVS 页的只读预检阻断；必须先取得每设备可恢复的真实源事实并设计一次性迁移，不能直接擦写或由新空 NVS 冒充迁移成功。签名装槽和无网络启动也不覆盖正式 Wi-Fi、FRPS、MQTT Broker、OTA HTTPS、64 KiB AEAD 工作流、物理 Flash 时延与实板恢复。此表仍是**软件候选**；P6-03／P7-01／P7-02 不据此验收。
