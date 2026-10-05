# Base 消费 MQTT 按实际长度分配的软件检查点

2026-10-02，Base 主树采用了下述受测 manifest 与官方生成双目标锁，三份输入逐字节一致；此检查点只证明软件消费，不构成实体联合容量或发布验收。验证过程没有访问实体板、串口或 `mac-ci-2`，没有修改 SDK、Wi-Fi 默认值、合法容量与协议上限。

## 受测输入

- Base 完整导出提交：`6882effd90aea8ba6358ddff67fadabb28f56550`。源码来自 `git archive`，不带主树 WIP。
- 候选 MQTT 提交：`6443b71db761f4d667503f14108687bad5e6b5ee`；只更新 `firmware/components/device_protocol/idf_component.yml` 的精确版本。
- 官方 `idf_component_manager.core.ComponentManager.update_dependencies` 清除旧 native 锁后，分别通过 C3/ESP32 普通产品 `idf.py reconfigure` 生成 `firmware/dependencies.lock` 与 `firmware/dependencies.lock.esp32`。未手改锁文件。
- 两锁的依赖变更只有 MQTT，另有 manifest hash 变化；其他依赖、target 与顶层锁合同保持一致。
- `firmware/tests/nvs-capacity-probe/dependencies.lock` 与 `.esp32` 不含 MQTT，仍与该 Base 提交逐字节相同；本轮未重新生成或验收探针锁。

固定 SDK 为 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与独立 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。本机与远端 SDK 父仓原生 lwIP gitlink 为 `c6f2f878e7b0f86033214b85547d579be43351e3`，父仓只因既有独立 lwIP 锁呈现该 gitlink 差异；不能声称父仓全树 clean。lwIP 自身 HEAD、771 个 tracked 文件的字节与执行位均匹配 2758，工作树、未忽略及忽略的 untracked 均为空。本机普通构建与远端签名构建的每个 target 各 80 个 lwIP 编译单元都来自 exact 2758。没有重写 SDK。

## 软件结果

| 目标 | 普通完整构建 | 普通 ASan/UBSan host | 既有私有测试输入签名完整构建 | 官方验签 | 签名 ASan/UBSan host |
| --- | --- | --- | --- | --- | --- |
| ESP32-C3 | 通过 | 通过 | 通过 | espsecure v2 通过 | 通过 |
| ESP32 | 通过 | 通过 | 通过 | espsecure v1 通过 | 通过 |

普通镜像为 C3 1,051,984 B、ESP32 986,928 B；签名镜像为 C3 1,183,744 B、ESP32 1,114,100 B，分别未超过原有 `0x130000` 和 `0x120000` 应用槽。签名只复用前序 RSA/ECDSA 私有测试密钥、公钥、配置与 policy，不创建新密钥。签名输入保留私有测试用途，不据此宣称生产发布。

完整受管运行源码和构建输入逐文件匹配其公开提交：MQTT 35 文件、FRP 40、OTA 11、Container 25。读取既有 `compile_commands.json` 后，普通与签名两类构建的两个 target 各有 MQTT 7、FRP 15、OTA 4、Container 10 个实际编译单元，全部匹配对应公开提交。文件等价计数与编译单元计数分别记录，不把未编译文件算成编译单元。

其他 provider pin 保持为 FRP `989cc876d92b815aeb0b6806fb861f0ee2b39a86`、OTA `04acb5e80a744649f8442607fb8d901d30880ca0`、Container `2b93b979b8b0760dcb96b28ac5d13fc52ae547bf`；WAMR 和 cJSON 锁未变化。Wi-Fi 静态 RX 缓冲与 BA window 都为 6、main stack 为 6144 B，C3 两项 Wi-Fi IRAM 优化仍关闭。MQTT 4096 B 上限、三条排队加第四在途、分片与过期、证书/时间、官方 core/outbox 合同没有降低。

## 制品与证据

完整 53 项软件冷归档保存在 ESP Tool 受限、Git 忽略的 `c3-validation-20261002/mqtt-sized-owner-base-software`，SHA-256 为 `3d70558704e875902e4465d692bc30d545b6a0d6a1a181a2e27cf2c71a8ae799`；内含已核验的 38 项签名归档。三份消费输入 SHA-256 分别为 manifest `4e339b78df090c997472a31e1efce4606d56302eee1005fe31d123701b565e54`、C3 锁 `5209a34060e428fec2fd4ae09ed3b0de0b32258a018ede2c9c4231d125bb4851`、ESP32 锁 `e56863477b15c4e1d8d4bdb892071ea086dc1410de718372286edb3eef55a509`。

- `generated_native_inputs.tar.gz` 包含 manifest 与官方生成两锁，SHA-256 `473bb98975814ac4c5977295e96412b4aacf4c9cec402b7d6c5116559214030e`。
- `base_mqtt_pin_candidate.patch` SHA-256 `9362ec8280a7ac2d555c0748c435536954958850b038345b0fa6f6ebfb356348`。
- 远端签名软件归档 `signed_software_evidence.tar.gz` 有 38 项索引，SHA-256 `faca0a22feb8644077339c40eaa84b04c91fe42796947ee01e3d9e0d177224aa`。
- `provider_equivalence_receipt.json` 记录完整 provider 运行源码/构建文件；`ordinary_compiled_provider_equivalence.json`、`signed_compiled_provider_equivalence.json` 记录实际编译输入。
- `ordinary_lwip_source_equivalence.json`、`signed_lwip_source_equivalence.json`、`sdk_source_state_receipt.json` 记录 SDK 双锁与源码回读；`ordinary_build_receipt.json`、`signed_build_receipt.json` 记录镜像、构建、host 与验签日志摘要。
- `native_lock_generation_receipt.json` 和 `nvs_probe_locks_receipt.json` 明确锁生成与未覆盖范围。

准备过程保留了错误与修正：首次 ComponentManager 导入路径错误、系统 Bash 空数组与 nounset 冲突、provider 初始范围误含 Component Manager 排除的 CI 元数据、SSH 连接中断，以及首次 SDK 父仓无差异断言与既有独立 lwIP 锁冲突。这些问题均未修改产品源码或 SDK；原远端构建进程在 SSH 中断后继续并完成。最终依照精确 SDK 双锁、tracked 字节/执行位与实际编译输入完成核对。

## 尚未完成

本检查点受测对象是 Base `6882effd90aea8ba6358ddff67fadabb28f56550` 的完整导出加唯一 MQTT pin／原生两锁变更；主树已经采用这三份相同字节，运行源码和另外两份 NVS 锁保持。软件冷归档不回写为实板证据。

本轮没有真实 C3 OTA 下载或联合资源采样。软件通过不能证明 MQTT 实际长度分配让普通 C3 满足 `49152` B 容量门槛，也不能证明满规格包、fragment 最大负载、FRP、TLS 与 Container 的实体联合容量已闭合。
