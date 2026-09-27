# P6-03 五仓精确锁双目标签名容量检查点（2026-09-27）

本检查点只归属已推送的 `esp-base@087f9baaa4414a75106103b46249c80b61189564`。在 `mac-work-1` 固定 SDK 的仓外目录，从该提交的干净 Git 归档分别建立 C3 与 ESP32 副本；未修改本仓正式分区 CSV、源码、锁或设备。测试键只在仓外目录读取，生成的镜像不是生产签名或刷写候选。

## 精确输入

- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，其中 esp-lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`。
- 唯一正式 manifest 中，MQTT 为 `c0677e5e779c3e51e814f2920420be7ec54f1d88`，OTA 为 `d98361f348e19e965efd7462277dde0ae13056fa`，FRP 为 `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`，Container 为 `6ef74faabb675bce0180570f5bdf0232af11106a`。四项 SHA 分别与各自公开 `codex/mqtt-runtime-audit-20260927`、`codex/ota-url-authority-preflight-20260927`、`codex/frp-header-only-feed-20260927`、`codex/container-classic-deadline-20260927` 分支的远端精确提交相同。
- C3 `dependencies.lock` SHA-256 为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`；ESP32 `dependencies.lock.esp32` 为 `5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`。两锁除 `target` 外相同，且均精确固定上述四项以及 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。两次构建后，活动锁 SHA、五项版本和五个下载组件的 `.component_hash` 均与锁一致。ESP32 在**全新归档副本**中先把正式 `dependencies.lock.esp32` 复制为该构建的活动 `dependencies.lock`，然后才配置与编译。
- C3 使用已记录的仓外候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`、几何一致的 `sdkconfig` SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`、RSA v2 测试键 SHA-256 `0dab19dd6a1ebe8ee5a2d634a68e7724bf9d44b827264413988dca1ffb0f6179`。仅归档副本的 C3 OTA policy 两宏按此前候选改为 `ota_1@0x140000/0x120000`，修改后头文件 SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`。
- ESP32 使用已记录的仓外 scratch 候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`、`sdkconfig` SHA-256 `6ddf140b2845f4e1d4d9bc69b4aad06f446715d7b3d41bfe164b1846a3e0d456`、ECDSA v1 测试键 SHA-256 `d6c9a9640b41ab421c225fb55ba44d5198ce7c647e64e96bd743cb4470637e09`。未拼接新的布局或签名配置。

## 官方构建与验签

固定 SDK 完整 `idf.py build` 分别完成 C3 `1128/1128` 和 ESP32 `1152/1152`。官方 `check_sizes.py` 以真实构建分区表和签名 app 执行容量门；`espsecure v5.4.0` 验证 C3 RSA v2 app、ESP32 ECDSA v1 app 与 ESP32 ECDSA v1 签名分区表。官方 `gen_esp32part.py` 解码两份构建分区表并验证 4 MiB 几何，双 `0x120000` app 槽为 `ota_0@0x20000`、`ota_1@0x140000`，包区为 `product_pkgs@0x260000/0x186000`。两份配置均启用测试产品 ID `esp-base-capacity-test`，ELF 实际链接 Container 产品入口、FRP、MQTT 与 OTA 实现。

| 目标 | 签名 app 大小 / SHA-256 | 每个 app 槽剩余 | `esptool image-info` 非填充尾 / PADDING 段 | 分区表 SHA-256 |
| --- | --- | --- | --- | --- |
| C3 RSA v2 | `0x111000` / `64902674ee7786dad20d18bbaf65499df191f77bcc40a578bfc8e448487b34a6` | `0xf000`，61,440 B | `0x10d15c` / `0x2e6c`，11,884 B | `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25` |
| ESP32 ECDSA v1 | `0x10fff4` / `09335ad8a7c0306f03a1edaa8fcd633e4000069850af1f68f937268c5361bb6d` | `0x1000c`，65,548 B | `0x100b4c` / `0xf42c`，62,508 B | `49f7ee4e5b70b3bd12cb68e3e10a8ab6d0ac180c24cfa4951b8326c28e2fd602` |

C3 分区表由官方生成器解码并与既有候选哈希一致；该签名方案只要求 app RSA v2 验签。ESP32 分区表的 ECDSA v1 验签输出为 `Signature is valid`。PADDING 是本次镜像里的实际填充段；双槽剩余不是可任意增加的代码字节数，后续改动须重新构建验签。

## 证据与未验边界

原始完整构建、官方验签、`image-info`、分区解码、锁与制品，以及机器校验收据在 `mac-work-1:/private/tmp/esp-base-087f9ba-p603-signed-20260927/`。最终 ESP32 结果取自其中 `esp32-locked/`，C3 取自 `c3/`，汇总为 `verification-summary.json`；`pristine/` 是未改动的 `087f9ba` 归档。

这验证了**该精确锁与这两份仓外候选布局**的离线静态签名容量。C3 正式表仍是双 `0x1e0000` 槽，ESP32 正式表未启用此 scratch 候选；未修改它们。没有运行实体板、真实产品包 guest、FRPS/Broker/HTTPS/OTA 同时活跃、动态 RAM 峰值、旧 AT/NVS 迁移、物理 Flash 暂存或掉电恢复。P6-03 的运行资源与设备验收仍未由此关闭。
