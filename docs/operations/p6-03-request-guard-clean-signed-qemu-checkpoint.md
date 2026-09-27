# P6-03 命令去重表精简后的双目标签名 QEMU 检查点

2026-09-27，从已整合的 Base `76e2974211bbf560cb207f5dacdffbb98523f752` 建立仓外隔离副本，复测真实 ABI 2 counter 包随签名固件启动时的堆和产品线程栈。测试只在 `mac-work-1:/private/tmp/esp-base-p603-guard-qemu-20260927/` 运行，使用测试键、合成 4 MiB Flash 和 QEMU；没有接触实体设备、正式凭据或正式分区。原始输入、构建、验签、UART、GDB、Flash 与逐文件 SHA-256 清单均保留在该目录，清单 `evidence-sha256.txt` 的 SHA-256 为 `0303e3451224c9766f914223b157d4fed4c392b747471c5ebb3af5bf653e8777`。

## 纯净输入与旧证据边界

固定 SDK 为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、其实际 lwIP checkout `2758df4cd3666b3b2a5b53830148379326425c0d`；根 `sdk-lock.json` SHA-256 为 `a0f3c1ab2c7a9d12c5f13361b8bed7f9c33171e2c971090fdf4bbbac726c18dd`。C3／ESP32 `dependencies.lock` SHA-256 分别为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`、`5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`，锁定 Container `6ef74faabb675bce0180570f5bdf0232af11106a`、FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`、OTA `d98361f348e19e965efd7462277dde0ae13056fa`、MQTT `c0677e5e779c3e51e814f2920420be7ec54f1d88`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。IDF 组件管理器从该锁重新获取组件；四个本地源仓与取得的 Container／FRP／OTA／MQTT 组件保留文件逐个 SHA-256 相同，缺少的只是组件管理器未装配的 CI／测试文件。WAMR、cJSON 是按锁全新获取，六份 `.component_hash` 均与锁匹配。完整比较结果为 `clean-input-audit.json`，SHA-256 `afa7ea6d803af78beeb1f4f44440395ef820191c05a0566a7d8876b9c9c14457`。

Base Git archive 的 238 个文件逐个核对：无缺失，仅仓外 ESP32 候选分区 CSV 与 C3 OTA policy header 两处内容变化，额外只有 C3 候选分区 CSV。ESP32 候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`；C3 候选 CSV `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`，与其 `ota_1=0x140000`／`0x120000` policy SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944` 对齐。两个签名 `sdkconfig` 位于仓外，SHA-256 分别为 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`、`ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`。这三处候选布局装配没有写回 Base 正式配置。

排查中发现，最初复制的仓外 `managed_components/esp_container/src/slot_runtime.c` SHA-256 `42f2915708d363935a6044e31e0f4779061617b52b1a72e4a16c36a42a6b8de1` 含 `P603_SAMPLE` 插桩，虽有正确的 `.component_hash`，却不同于锁定源码 SHA-256 `4a1699dacace223c92da2b30542fb7b16486e7612d49fb6c285586fb1e68a7a1`。因此最初本轮构建的 C3／ESP32 app SHA-256 `da1db6a3b58b1405789bc9c24bc190bf3ea0343b9bd7054da937eda4edabfa95`／`c6b2e95531fb9945f59130a8d49f37fc9e392ec94c2474d391ab6c105d0f4391` 均弃用，未进入下列种子或读数；错误输入保存在 `contaminated-managed-components/`。此前[产品验包工作区移栈检查点](product-workspace-stack-checkpoint.md)的两份签名 QEMU 镜像也含 Base／Container 诊断采样：旧 ESP32 `49,100 B`、C3 `51,180 B` 只属于诊断镜像。旧 C3 签名 app SHA-256 `2f46227691aa5102ce4176a8d524e70feb33f68c3788db560e118404909f728c` 对应 ELF 含 Base `p603_sample` 和 Container `p603_slot_samples`；旧 ESP32 签名 app SHA-256 `c8d6f40a62a35bb25d497ac52a1745590859207233ddbd2fe6a8bd401903ab83` 对应 ELF 含 Base `p603_sample` 与 Container `P603_SAMPLE`。旧两份普通构建的 ELF／map 无上述采样标记，其 map SHA-256 `d4d75478325a3ee4e5c3091d4135df779a77de098f1f037088b735b24af173d5`／`48182b67fa50827cc0a661f559263d586cba2497c4344b6c16401659dc82ff8e`，本轮 guard 静态 `.bss` 减少 2,304 B 的独立证据不受此污染；Base host ASan/UBSan 使用本仓源码和假件，也不使用该仓外组件副本。**旧诊断镜像与新纯净镜像的堆值不可直接相减来归因 guard 改动。**

## 构建、验签与 ECS2 种子

固定 SDK 的两目标纯净签名构建均通过，纯净 ELF 中无 `P603_SAMPLE`、`p603_sample` 或 `p603_slot_samples` 标记。官方 `espsecure v5.4.0 verify-signature` 验证 C3 RSA v2 app 的 block 0、ESP32 ECDSA v1 app 和签名分区表；原始验签日志 SHA-256 依次为 `2cc0922cae6d1eaf04f0cae91fa5b83fed39afdc6d5b2f56cf1bc922dc975cff`、`3d4c27b901e82c0e6d5a10e896068f7d5937b51aa76ba48e80cbdbdbbfa3bff6`、`31b535b01e4bddcb492cd132bb200c6c78b801477ff0bf78a88e6a5d6e2a326b`。

| 目标 | 签名 app 长度／SHA-256 | ELF／map SHA-256 | ABI 2 包 SHA-256 | ECS2 slots SHA-256 | 种子 Flash SHA-256 |
| --- | --- | --- | --- | --- | --- |
| C3 | `0x111000`／`99fa4ce80a33b54e6992db7e31c84523096bbc353c351c2593af3505c19d1878` | `371c91c1e76777d5b13bbdb538064debfe96d38fda569bf9272feccdd58635e1`／`f315e8c6d9bdf1fbd5ab54f362d3f29444fadebcc08709856efd0592a62371cb` | `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c` | `925417ac67488eec1d5966a5b16330859da056e4693e5e436e877b5bab6394c0` | `2fff290c2e86a4f04860f49c678acefa03d09e083f6904240efb0c0144209bd0` |
| ESP32 | `0x10fff4`／`9403d6243a98a6eca8b91d3452e4900e348e217c6b985495e84b8ca001f2b7a6` | `cc7819e9b2a71f617a2a92feebee975d9067de13f3c26e104d3cb401b028e0b4`／`ede4fff776f86bee101045245e38aa3d04163461bf0df4beca838acc6ff48b19` | `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e` | `12d8536956f8bbaff8a98f1d7fd71a038ee6f38c64ee41bb8e27421a9b1babf1` | `ca1a29533293ec1cbab30bfb1dc2ec44d4ad6b563b3d5fe685c035571baf4663` |

两份 ECS2 sequence 6 种子均使用本轮新 app 的完整签名摘要重新生成，填入各自测试包、`frp_scratch` 与 `base_store`；原始装配脚本、逐区偏移和 hash 位于 `prepare-clean-flash.py`、`*-clean-flash-manifest.json`，其 SHA-256 见同目录 `evidence-sha256.txt`。两 app 均位于各自 `0x120000` app 槽内，C3 尚余 `0xf000`、ESP32 尚余 `0x1000c`。测试签名键与 eFuse 仅用于仓外 QEMU。

## 启动读数与 Flash 读回

以 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`（数值 `2052`）在 Base `READY` 前一行通过 GDB 调用固定 SDK 的 `heap_caps_get_free_size`、`heap_caps_get_largest_free_block`、`heap_caps_get_minimum_free_size`。C3 QEMU 的应用日志走 USB Serial/JTAG，不在 UART0；只对 QEMU 未模拟的 ADC2 校准在 GDB 中跳过一次，随后命中 `esp_base_container_product.c:482` 的产品 `RUNNING` 成功分支，`open.sequence=6`，再命中 Base `READY`。ESP32 的原始 UART 同时记录 `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0` 和 `ESP_BASE_READY ... container=running`。

纯净镜像未链接 `uxTaskGetStackHighWaterMark` 符号。为不重建或插桩，GDB 在产品线程 `RUNNING` 处只读 `xTaskGetCurrentTaskHandle()` 返回的 `pthread` TCB，从 `pxStack` 按固定 SDK `tasks.c` 的 `tskSTACK_FILL_BYTE=0xa5` 与正式高水位算法数连续未用字节。两目标配置栈均为 16,384 B，表中栈值为该启动路径到 `RUNNING` 时的最低未用字节，不代表后续 event／stop 最坏深度。

| 纯净签名 QEMU | `READY` 堆 free | 堆 largest | 启动堆 minimum | 比 48 KiB 门多 | 产品线程栈最低未用 |
| --- | ---: | ---: | ---: | ---: | ---: |
| C3 | 60,384 B | 45,056 B | **53,208 B** | 4,056 B | 5,140 B |
| ESP32 | 55,236 B | 43,008 B | **51,456 B** | 2,304 B | 5,012 B |

ESP32 另一次同一纯净镜像启动在 `READY` 测得 free 55,244 B、largest 43,008 B、minimum 51,464 B；表中取两次较低 minimum。C3 独立纯净运行也复现相同 53,208 B。新栈水位碰巧与旧诊断镜像的 5,140／5,012 B 相同，现由纯净镜像单独取得，不能把旧证据当作新测量。

| 目标 | 最终原始 GDB SHA-256 | 最终原始 UART SHA-256 | QEMU 后原始 Flash SHA-256 |
| --- | --- | --- | --- |
| C3 | `bdac5ba9ef074335706d5f4fb96f08d7889d9eb8b43de42240cdc409fa4e9dda` | `fc42799a2fea156670cac0d95f618743a62f34a4b5784ab5c7a1fa1e02b8ad05` | `7b96d940cae0778efce185742bd28184bea3b58572a49bbdb900c716545d62ff` |
| ESP32 | `c8fa0e5a0239487409be612d303138abc4b5e6c99cc1144986565e7882766be8` | `6ffddc95debb32564dfc08a00f26956c2ad222f7c5d40c8c226c1e73ac326b7e` | `266a4f5dac80a4ef0978ed133668198a8019e337aa1414167cb1ca9657834fd9` |

`clean-flash-readback.json` SHA-256 `e367f85a682bd9aadc1dc103351ad9e477c37ffa964af273a02a2dc8a7464dee`：两目标 QEMU 后 app 槽、ECS2 包区与 `base_store` 均与种子逐字节一致；`frp_scratch`、设备 NVS 与 otadata 有预期运行写入。上述表的 UART、GDB、Flash 都是原始文件；命令行 JSON、eFuse 副本、QEMU 自身日志、构建日志及其 SHA-256 也在证据目录。

## 结论边界

两目标的**无真实网络连接**签名产品启动切片均高于 48 KiB 内部堆 minimum 门，且各自 largest 高于 24 KiB 单块门。QEMU 命令虽装有 OpenETH 虚拟 NIC，ESP32 状态报告仍是 `wifi_state=unconfigured`、`time_ready=false`、`mqtt_state=unconfigured`、`frp_state=unconfigured`；合成配置没有真实接入凭据。此处未测 FRPS TLS 会话、Broker MQTT、HTTPS OTA、guest 事件与 stop 并发、实板调度或恢复。静态 guard `.bss` 减少 2,304 B 已另由[命令去重表容量检查点](p6-03-request-guard-capacity-checkpoint.md)证明；本轮纯净启动通过不能推导五能力并发通过，P6-03 整体仍待验收。
