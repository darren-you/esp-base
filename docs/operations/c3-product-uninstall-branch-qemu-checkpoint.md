# C3 产品卸载版本的签名包 QEMU 启动检查点

2026-09-27，以 Base `bdf164750410f7a7f030fa3dbd188ae94c4823c3` 和锁定的 Container `3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd` 在 `mac-work-1` 独立目录复建测试键 RSA v2 应用。仓外三槽合成 Flash 预置真实签名 ABI 2 counter 包；两次 QEMU 冷启动都经正式 Base 产品启动线程完成 guest `init`、进入 `RUNNING` 并到达 Base `READY`。本次没有调用产品卸载、安装、事件投递或正式设备写入。

## 输入与候选几何

| 输入 | 精确事实 |
| --- | --- |
| Base 源 | `bdf164750410f7a7f030fa3dbd188ae94c4823c3` 归档 SHA-256 `bc1f8b2d91becfa8b775d750b159a28c0f2a93d5b1c58757d9901883e525f050`；产品绑定源码 SHA-256 `c5daf2f661c1e13b2a85ebf03692099ed6ff58caa3405a924adc55f6c29878bd`；`firmware/CMakeLists.txt` SHA-256 `02295ea5e6a5edfe7524dfbf73ddf3bd6ad3908d8df2f301606a213927a425b9` |
| SDK 与组件锁 | ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；Container `3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd`、FRP `98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `7f316c2a3a71dcae234b046905aee60696a357d3`、WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5` |
| 仓外构建输入 | C3 sdkconfig SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`；候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`：`ota_0@0x20000`、`ota_1@0x140000`，各 `0x120000`；`product_pkgs@0x260000/0x186000`，三槽各 `0x82000`；`base_store@0x3f6000/0x8000` |
| 测试策略 | 产品 ID `esp-base-capacity-test`、key ID `capacity-rsa-20260927`；包验签公钥 DER 398 B、SHA-256 `ac352916cc880c8ae78273f975647b68db5e58ad63288ec2071b826a7a8c0c55`，与 sdkconfig 内十六进制公钥逐字节相同。应用 RSA v2 签名键与包签名键均为仓外测试键 |

**条件性源码差异必须与 Base 提交分开看。**公开 Base 的 `esp_base_ota_policy.h` SHA-256 为 `831ece0b19a7c2d2a0c8078b4934152c5074140f38df1b0bc9ddb34958f7401b`，其 C3 固定 policy 要求 `ota_1@0x200000/0x1e0000`。仅在本次隔离构建副本中，将 C3 分支的 `ESP_BASE_OTA_1_ADDRESS_BYTES` 从 `0x200000` 改为 `0x140000`、`ESP_BASE_OTA_SLOT_SIZE_BYTES` 从 `0x1e0000` 改为 `0x120000`，候选头文件 SHA-256 为 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`；ESP32 分支未改。此前 [C3 签名包检查点](c3-signed-product-qemu-checkpoint.md)也使用这一候选差异。本仓正式 policy、分区 CSV、实体板及 eFuse 均未修改。

遗漏这两行时，未经候选改写的签名 app SHA-256 为 `0d883809875b6c0a7309666dccef41e391d21c07bc7ff438e573f8b5db1e263b`，与其一致的 ECS2 blob SHA-256 为 `d706c2cc742a8d1fa8cd11acdca6b84eba66c3c9d89f874f183c97489a44669c`，合成 Flash SHA-256 为 `36c07e1f5fc0c3a71e07ee85a9137137d5d96cd3a293784172f216b698037e5a`。QEMU 仍在读取 ECS2 前阻断：GDB 读得 `running/boot@0x20000/0x120000`、`target@0x140000/0x120000`，但 OTA policy 期待 `target@0x200000/0x1e0000`；`eota_observe_slots` 返回 `EOTA_UPDATE_SLOT_UNAVAILABLE=3`，Base 产品初始化返回 `ECONTAINER_SLOTS_UNCERTAIN=4`，启动结果为 `BLOCKED=1`。失败输入和日志已独立保留；它是候选分区与正式 policy 不一致的证据，不能当作 Container ECS2 读取失败。

## 签名包与合成 Flash

候选改写后，官方尺寸门通过：签名 app `0x111000`／1,118,208 B、SHA-256 `c994cfc1ae04095c72cf5f2bc65c9af1cdaba36e958976d4f0f9fdf13cdb9a3f`，每个 app 槽余 `0xf000`；固定 SDK 的 `espsecure.py verify-signature --version 2` 验证 RSA 签名成功。包由 Container `3b5f16f` 的 `product_package.py`（SHA-256 `d0bcf846e168b0a173c1c866b8b98c09f99a471cf5a3c50682fe2901f812ed51`）签名并验签：ABI 2 counter Wasm 469 B、SHA-256 `b9422cb4cb72983141988c4a9a59b602026d94729e2362403a04d1723f98a739`；`product.pkg` 10,240 B、SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`。

仓外 seed 源码 SHA-256 `2e4071952aef3b913042ab7d6edf5365c9b0282460a3a5c90ac4039b8a35c2b6`，以锁定 Container `3b5f16f` 的正式 `slots.c` 和包 validator 为主体，内存 IO 承接测试 Flash／NVS。它按 `initialize → reserve → write_and_prepare → begin_trial → mark_healthy → confirm → reconcile` 生成 sequence `6`、`CONFIRMED`、slot `0`；seed 输出的运行固件 SHA-256 **就是上述新签名 app 摘要** `c994cfc1ae04095c72cf5f2bc65c9af1cdaba36e958976d4f0f9fdf13cdb9a3f`。ECS2 blob 288 B、SHA-256 `ac23b76b6222dc80556ccc6596b2d303f6968c9c7c8cf6d043d3e421f2286ffe`；包分区 SHA-256 `0c612b191505e8f8e0bdd324059474c262ed94184bdeb4debe1350c6bc3ea51f`，slot 0 与原包逐字节一致。

固定 IDF 的 NVS V2 生成器将 ECS2 写入 `base_pkg/slots`，`base_store` 32,768 B、SHA-256 `654898e565dd6fa471eb9f93f36c9a2fc1d159fc4c03b6449ebebf277eb37b4d`；官方 NVS parser 报活动页 `CRC32: OK`，其余页 Empty。全 `0xff` 起始的 4 MiB 合成 Flash 按官方 `flash_args` 放入 bootloader、候选分区表、初始 otadata、上述签名 app、包区和 NVS；每段在写后按源字节回读，`ota_1` 与 FRP scratch 仍全 `0xff`。分区表 SHA-256 `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25`，预置 Flash SHA-256 `e0dbe311329665c93e7cddd3d5326f293b3da9c8ed4de20df670da959735b7ce`。

## 两次产品启动与回读

固定 Espressif RISC-V QEMU `esp_develop_9.2.2_20260417` 使用独立端口 `23366`、`23367`。两次冷启动各只在 ADC2 校准入口由 GDB 跳过 QEMU 无法完成的模拟校准一次。GDB 在 `esp_base_container_product.c:481` 均读得 `econtainer_product_init=ECONTAINER_RUNTIME_OK`、sequence `6`，随后命中 `esp_base_main.c:387` 的 Base `READY`，`s_boot_storage_claim={owner=0, token=0}`；本地失败断点未命中。稳定 idle 任务上使用 `heap_caps_get_free_size(0x804)`／`heap_caps_get_largest_free_block(0x804)`：首次 `57,048/28,672` B，二次 `57,056/28,672` B。这里是整条产品运行路径的空闲时刻值，不是纯 WAMR 分配量或网络并发峰值。

第一次运行后的 Flash SHA-256 为 `c0ec5f2386d388185e0be6671150d5fac7b18d2477be72bb5aff1953ed472c4a`；第二次以这片运行后 Flash 冷启，结束后仍为同一摘要。逐字节比较确认相对初始预置只改变系统 `nvs@0x9000` 的 101 B 与 otadata 的 12 B；签名 app、`ota_1`、包区、FRP scratch 和 `base_store` 均未变。两轮提取的 `base_store` 再经官方 NVS parser 验证活动页 CRC OK；eFuse 副本 SHA-256 始终为 `2054600a17c72426ac024ae851e7ea26f9cf612f31140b445ff713ba15ac09c8`。原始输入、GDB 脚本／日志和逐区摘要收据只存于 `mac-work-1:/private/tmp/esp-base-bdf1647-uninstall-qemu-20260927/`，私钥、包与 Flash 不入 Git。

本检查点只证明该**条件性候选几何与签名测试键**下，最新 Base 产品启动入口能读取仓外预置的 ECS2／包，完成 Container 验签、guest init 和重启读取。它不证明公开安装入口、guest 业务事件、卸载 stop/join/reopen、FRPS／Broker／OTA 并行、正式分区迁移、掉电或实板验收。旧 `d3144b3` 检查点的 app／包／ECS2 SHA 与本轮不同，不能拼接成一条运行证据。
