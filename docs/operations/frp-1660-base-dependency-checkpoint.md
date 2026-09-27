# FRP 1660ac2 精确消费检查点

2026-09-27，从 Base `335b4cdb3a62936d523f29dda0f7f3dccea0d505` 的独立工作树出发，仅将主固件的 FRP Component Manager 声明和 C3／ESP32 两份生成锁改为公开 `esp-frp@1660ac2d5a0d607a1e6b2fd278abeb6d93d56641`。该提交的远端分支 `codex/frp-session-flash-reader-20260927` 精确指向此 SHA。相对旧锁 `98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`，FRP 的 `idf_component.yml`、`include/`、`src/` 无差异；新提交增加宿主／QEMU 测试与证据。Base 不修改 FRP API、产品分区、运行策略或真实设备。

## 输入与解析

- 构建宿主为 `mac-work-1`；固定 `IDF_PATH=/Users/darrenyou/.cache/darren-space/esp-idf-578cf89`，SDK 提交 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。非交互 shell 先把 `/opt/homebrew/bin:/opt/homebrew/sbin` 放入 `PATH`，再加载该 SDK 的 `export.sh`。全部隔离输入与原始日志在 `/private/tmp/esp-base-frp-1660-pin-20260927/`。
- 输入是 Base 父提交的 `git archive`（SHA-256 `dfd20dde1896b4e918406b6ead3913f9c3871ef34423fdca740fb336610ac37c`），再覆盖唯一已改的 `firmware/components/device_protocol/idf_component.yml`（SHA-256 `2b8b3652d65db49ce28b2f7bc3b91f9c79c13973c8b771c38b5811839d1cd353`）。旧目标锁只在仓外隔离副本删除，由官方 Component Manager 重新求解；没有手填组件 hash。
- 两目标生成锁分别为 SHA-256 `10314c4150218398b7faaedd2e3b61401a33683168d5e7cadd9fe08e1d15922b` 和 `b331aa914b4ac12f582d105240aca03dd0893c5af5543a1a270cc9afe908a2e8`，均锁 `esp_frp.version=1660ac2...`、组件 hash `39b23c14c3715acc13afafa4984207987927b3eb30ee05b8938461121e09762a`、manifest hash `78bf99d5f5cdb0c19af33dfce694d1261e8e2540af250a1efa51c62512998684`；target 分别为 `esp32c3` 与 `esp32`。受管组件中新测试文件 `tests/session_idf_flash_fixture.c` 与该 FRP 提交同为 SHA-256 `b24526c6b0687c1515558b6642f3f2db77f4ca70481d6e4d3ae6cc4232376baa`。两目标 `compile_commands.json` 各列出 15 个 FRP 源翻译单元，含 `src/client.c`，证明新解析组件实际参与编译。
- NVS 容量探针只直接消费 Container `6ef74faabb675bce0180570f5bdf0232af11106a` 及其 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`，其清单和两份锁没有 FRP 项；因此不向探针虚增 FRP 依赖，原锁保持不变。隔离副本按六页、stage 1 分别完成 C3／ESP32 全量构建，镜像 SHA-256 为 `99666fe58c2687616934ab9a71a2e1388cebd50b940b41e585baa6a0bc5ffee3`／`6c077aed64d5ea94e08c786a0f87e337cb51bb2617602bec14bfa5bdb4a3ec1e`；两份探针锁与源仓 SHA-256 逐字节相同，分别为 `8c38d913226b453d1af09c80df087f6d5aae882cba3abcc1c80d7956028b759c`／`55d15dd0112df5c9e265bb3ef580d01d3f735eb49ab929173d07ae8fb16bac29`。此处没有重跑三阶段 NVS QEMU 容量写入。

## 固定 SDK 构建和宿主回归

| 目标 | 主固件普通完整构建 | 签名产品完整构建与官方容量门 |
| --- | --- | --- |
| C3 | `0xdefb0` B，SHA-256 `c7657385f7875925a959865d85cd861989488c91b537b1f4f40b642d4d966d98` | 既有仓外 RSA v2 测试签名配置 SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`；仅在隔离源码使用 `c3-frp-scratch-candidate.csv` SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`，并把 C3 OTA 槽策略改到该候选几何，策略头 SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`。app `0x111000` B、SHA-256 `0f9419c3c732ba9a5205c6c332162249368421af67050c4e6d8c873a11023981`；`0x120000` 最小槽余 `0xf000`；官方 `espsecure.py verify-signature --version 2` RSA 验证成功。 |
| ESP32 | 使用显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON`，未签名普通镜像 `0xd2bb0` B，SHA-256 `1720e712391519f3e61e55ec4b08151bcc217db558d787d8a86befabf375a78b` | 既有仓外 ECDSA v1 测试签名配置 SHA-256 `943406f3351f9786567a684d03c152e59e1a175fd819d44eea1f9e8e56e303b3`，使用**当前正式** `esp32-partition-table.csv`，SHA-256 `f3f29e52f2ed0ccb3fbb3faf9e3d978d359aa9a958c2b3a6120399af70f11b73`；app `0x10fff4` B、SHA-256 `952b5d07d884b93398a388c36e26939bf016e9e9deb41a5be8c1e4e82663201d`；`0x120000` 最小槽余 `0x1000c`；官方 `espsecure.py verify-signature --version 1` 对 app 与签名分区表均验证成功，分区表 SHA-256 `0b22156f31a15128763fae584490a1100f1b0675c0e3f55ec159a62eb73e62c5`。 |

两目标 `firmware/tests/run_host_tests.sh` 在同一新受管组件源码下以 ASan/UBSan 跑完：C3 **20/20**、ESP32 **19/19**。原始日志为构建宿主上述目录中的 `host-c3.log`（SHA-256 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`）与 `host-esp32.log`（SHA-256 `e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`）。FRP 公开头文件与固件源码无变化，因此这些主机输出与上一精确锁测试的摘要相同；此处仍以新锁实际编译、运行的日志为证。

随后把 Base `6a26cbb67d244c5cd5378c51352368b235fd31a7` 的**仅测试**变更集成在此 FRP 锁之上；签名构建源码、配置、锁和上述容量事实均未更改。集成副本重跑 C3／ESP32 host ASan/UBSan，仍分别 **20/20**、**19/19**，日志 `integration-host-c3.log`／`integration-host-esp32.log` 的 SHA-256 与上段逐一相同。再用干净 Container `6ef74fa...`、WAMR `c10736f...` 与 wasi-sdk 33 执行 `run_container_lifecycle_test.sh`：真实签名 ABI 2 纯 Wasm 超期得到 `ENTRY_EXPIRED`、Base 启动阻断和清理、同进程新启动替身可重新打开；原有签名包 100 次安装／卸载回归完成 ECS2 sequence **1→601**，ASan/UBSan 无报告。新锁集成日志为 `integration-product-lifecycle.log`，SHA-256 `f5dc4fc546e9f3b98b83974a029c3936addc6627542e1a3e9c1c5852d6b11005`。宿主 Flash/NVS 与固件集合仍是替身，新的启动替身不证明同一实体 boot 恢复。

以上只验证精确源码消费、静态容量、签名和 Base 宿主替身路径。未建立 FRPS 会话，未运行新版锁的网络/guest 并发或 P6-03 复合场景；没有写实体 Flash、迁移 AT、启用硬件 Secure Boot 或证明实板验收。签名镜像使用仓外测试键，不是设备发布制品。
