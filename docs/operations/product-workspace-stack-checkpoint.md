# 产品验包工作区移入线程栈的双目标检查点

**证据勘误（2026-09-27）**：后续按受管组件锁复核仓外目录时发现，两端旧签名 QEMU 所用 `managed_components/esp_container/slot_runtime.c` 含未纳入 `esp-container@6ef74fa` 的 `P603_SAMPLE` 诊断插桩；C3／ESP32 签名 ELF 均有对应标记。下文 **49,100／51,180 B 堆低水、5,012／5,140 B 栈余量和 guest 运行结果仅属于这两份诊断镜像**，不得称为未插桩精确锁的运行容量验证，也不得与其他镜像做节省量推算。原始镜像、日志和摘要保留；纯净锁重签复测另行记录。独立 host 假件测试不使用该受管组件；两份普通构建未检出该插桩标记，但这不补足签名 guest 的源码一致性。

2026-09-27，P6-03 独立源码实验。输入 Base 为 `24b41752256b10c5887a92a5f2b4f3107b7077e5` 加本分支的四处源码／测试改动；固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，C3／ESP32 锁文件 SHA-256 分别为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`、`5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`。锁内 Container `6ef74fa`、WAMR `c10736f`、FRP `9a0839a`、MQTT `c0677e5`、OTA `d98361f`。仓外测试目录为 `mac-work-1:/private/tmp/esp-base-p603-workspace-stack-verify-20260927/`，仅使用测试签名键、真实签名 ABI 2 counter 包和合成 4 MiB Flash；没有访问实体设备、正式分区或产品凭据。

## 源码与调用寿命

Base 从常驻 `s_product` 移除 `econtainer_package_workspace_t` 与 `econtainer_wasm_workspace_t`，在唯一产品 `pthread` 内的 `open_selected` 栈帧声明两者，并把临时 `validation` 的指针传入同步 `econtainer_product_open`。Container `slot_runtime.c` 在同次调用中完成签名、Wasm 校验和 runtime open，返回前释放 Flash 映射；它把 `verified_info` 指针覆盖为自己的局部 `info`，所以 Base 删除无消费者的静态 `verified_info`。返回后 Base 不持有这两个栈对象的地址。两工作区合计约 **5,504 B**；产品策略的 owner pthread 栈下界随之从 8,192 B 提至 **16,384 B**。未配置产品时保留 `0`，host 假件断言 8,192／16,383 B 拒绝、16,384 B 接受。

固定 SDK 下，Base host ASan/UBSan C3 **20/20**、ESP32 **19/19** 通过，原始日志 SHA-256 分别为 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`、`e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`。正式分区、默认无产品策略的双目标普通编译通过：C3 `0xdefc0` B、镜像 SHA-256 `1b7950ff905c3faec872cd434aa536b1be604dc9dc668fb9fa3cfeef84269a78`；ESP32 显式离线普通构建 `0xd2bc0` B、镜像 SHA-256 `0aecb7231de41f74106e940b2b104711fca851439246b3c7ad5301971d4f4036`。普通构建不允许据此刷设备。仓外候选签名产品构建的无采样镜像也通过官方验签与尺寸门：C3 RSA v2 `0x111000` B、SHA-256 `14499ad70053bbd3db7d7af85cf463fa1cd631a4abd910a571b782b48f638e11`；ESP32 ECDSA v1 `0x10fff4` B、SHA-256 `f53b0ac28cfb1ebd39f6be4b4934901d39466a916f043665b255d1f78e9f2bca`。

## 签名 QEMU 栈与堆

为读产品线程真实低水，仅在仓外源码副本加入阶段采样。ESP32 采样补丁 SHA-256 `201f0a257c7bd6f87a5d0b976f57169e1910f225fbf81423b099349b6b17b1c6`；C3 补丁 `b9e68762e253d3b548954675d14be10951f9fa2816e943b4c8829acc327bfc59`。`uxTaskGetStackHighWaterMark(NULL)` 在固定 IDF 两目标的产品线程返回自创建以来最低未用**字节**；堆读数使用 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 的当前 free、largest 与 minimum。采样和日志会扰动栈、堆与时序，表内值只对应各自的已采样镜像。

| 仓外签名 QEMU | 最低未用产品线程栈 | 内部堆 minimum | guest 存活时 largest | 到达结果 |
| --- | ---: | ---: | ---: | --- |
| ESP32，16,384 B 栈 | **5,012 B** | **49,100 B** | 43,008 B | `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、Base `READY container=running` |
| C3，16,384 B 栈 | **5,140 B** | **51,180 B** | 45,056 B | 产品线程 6/6、Container 4/4 阶段，Base `READY` 断点 |

ESP32 使用仓外 `frp_scratch@0x3ea000/0x10000`、六页 `base_store@0x3fa000/0x6000` 候选表（CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`），`sdkconfig` SHA-256 `ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`。采样 ECDSA v1 app SHA-256 `c8d6f40a62a35bb25d497ac52a1745590859207233ddbd2fe6a8bd401903ab83`，原签名 counter 包 SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`；按新 app 完整摘要重建 ECS2 sequence 6 后，合成 Flash SHA-256 `3642b60a86349f5a35ae60d011c60f22a0cc1d482ac3edb3e6928a5e73de7d96`。UART 原始日志 SHA-256 `bd3a81b815431522848edfd3adb169eebc34d62429a0f089576824fe99124f11`，`READY` 后继续观测 11 秒。产品 open 前最低未用栈 8,788 B，验包前 6,948 B，验包后 5,012 B；WAMR open、init 后没有再降低。

C3 仅在仓外把现有候选 CSV 的双 `0x120000` app、三 `0x82000` 包槽与 OTA policy 的 `ota_1=0x140000`、槽尺寸 `0x120000` 对齐；CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`、policy 临时 diff `6d75d48de13df4cbf7109910a9b0be6dacf7a1225584c5ec35f09b4d8d02a07c`、`sdkconfig` `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`。第一次漏改 policy 时，真实 `eota_observe_slots` 返回 `EOTA_UPDATE_SLOT_UNAVAILABLE`，产品在 ECS2／验包前阻断；GDB 原始证据 `c3-slots-gdb.log` SHA-256 `e6e9683fbd3181bdc795640112eab91a2f62b4367c0f6c929ae6c9359697e529`。对齐后重新构建并官方 RSA v2 验签，按新 app 摘要重建 ECS2 sequence 6。采样 app SHA-256 `2f46227691aa5102ce4176a8d524e70feb33f68c3788db560e118404909f728c`，counter 包 SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`，合成 Flash `bacf5de81f07366969d60961b0c6857af173f7ca1ddee07b090c42b83230cfc8`，GDB `READY` 读数日志 `59a0cf5a48be85c26e8f56a56b722af8e6969b24218fffec6c1f5648026582a5`。C3 QEMU 的 USB console 不在 UART0；仅对 QEMU 未模拟的 ADC2 校准跳过一次，不修改固件正常入口或物理设备。产品 open 前最低未用栈 8,540 B，验包前 6,676 B，验包后 5,140 B；WAMR open、init 后没有再降低。

## 裁决

计划 §13.3 的内部 free 门为 **48 KiB＝49,152 B**。本次 ESP32 启动 minimum **49,100 B，仍低 52 B**；C3 候选 QEMU 高 2,028 B。两目标 guest 存活时 largest 均超过 24 KiB 单块门，但这只是无真实网络负载的切片。ESP32 之前静态工作区、不同 MQTT／OTA 锁下的无探针签名 QEMU minimum 为 43,636 B，不能把两次差值直接归因于本次移栈。当前 16 KiB 产品线程在受测启动路径上分别最大使用 11,372／11,244 B，仍缺 event、timer、stop、异常验包及实板调度的最坏栈深证明。FRPS 会话、MQTT Broker TLS、OTA HTTPS 下载与 guest 同时运行、真实设备及断电恢复均未测。本改动是独立分支的软件资源实验，**P6-03 容量门和五能力验收仍未通过**。
