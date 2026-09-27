# FRP 会话阶段复用的 Base 精确依赖检查点

2026-09-27。从干净的 Base `24582c348baf4062385fa326b6addfc80fc7ea50` 建立独立工作树，只把正式主固件的 `esp_frp` 从 `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3` 更新到公开提交 `0af12209ee731617e635684309c026ae6b49c5ae`，并重新生成 C3、ESP32 两份主固件锁。FRP 两提交间的源码差异只在私有 `src/session.c`：认证后 Flash reader 窗口进入握手／控制阶段联合区；公开头文件、组件清单、ABI、wire 和记录限额未变。上游测得单个会话对象减少 872 B；这个数字不能代替 Base 的运行堆峰值。

## 精确输入与锁

- 构建宿主 `mac-work-1`；`tools/check_sdk.py` 核对固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与实际 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 通过。
- 在仓外隔离源码副本分别删除旧 C3、ESP32 生成锁，再由官方 Component Manager `idf.py reconfigure` 重新求解。新 C3 锁 SHA-256 `d6fbd852c038d3dd3d62fa8afdf4c88520dcf007df300555d77d362051200b6d`，ESP32 锁 SHA-256 `6cbdf62f7230d90f0bd1925460ddb852eaeca64b04bbef77f7c9362790992b69`；target 分别为 `esp32c3`、`esp32`，共享 manifest hash `9438e333e8429fffdfb8cce51abd7d3dde04cc445b9d41f09f0867abe46cbf8a`。两锁相对父提交都只改变 FRP 提交、FRP component hash 与 manifest hash，没有手填哈希。
- 官方 `validate_hashfile_eq_hashdir` 逐一重算六份新下载受管组件的目录内容，并与 `.component_hash`、两份锁比对：Container `7552b87b4ff77cc236df95e208d6b4b3f1434444bcf3101a1987bd078453ae37`，FRP `03b1232bf0802553edc2b47982d9ab9c6d4640f389b22d466f1557b1e3fa3f05`，OTA `58dbb4b4cd22596ec9604b634b4f24f2417da77f66c57c5d0ed7ba6afce3693c`，cJSON `e788323270d90738662d66fffa910bfe1fba019bba087f01557e70c40485b469`，MQTT `7837059bb46e257033dde0d02cc9ae014267a5e09e9a1dbff7f36641d611dd36`，WAMR `713ac0a363a97ad951440e9f5a495d5cac426ee5501667a61d8aed6291c3facc`。FRP 受管 `src/session.c` SHA-256 `de398930de2e46cd1c9fbb4a5c63184fc16a625087caf245e858ed2c4e1ad82f` 与新提交逐字节相同。两份签名构建副本再次通过相同六组件内容检查，编译命令包含新 FRP session 与 Container 产品入口。
- NVS 容量探针不直接消费 FRP；其 C3／ESP32 锁保持父提交原字节，SHA-256 分别为 `8c38d913226b453d1af09c80df087f6d5aae882cba3abcc1c80d7956028b759c`、`55d15dd0112df5c9e265bb3ef580d01d3f735eb49ab929173d07ae8fb16bac29`。

## 本轮完整回归

| 目标 | Base host ASan/UBSan | 固定 SDK 主固件普通构建 | 仓外测试键签名产品 app |
| --- | --- | --- | --- |
| C3 | 20/20 通过 | `0xdefc0` B，SHA-256 `5c26fa9c60ef10e2cfa2305e91c6688738030f2d98f43eeb8212b04aa8bd205b` | `0x111000` B，SHA-256 `ee3a27c30adaf6ed68e7ab567adffab64a45685cfe6da44c686464438a03f5f6`；双 `0x120000` 槽各余 `0xf000` |
| ESP32 | 19/19 通过 | 显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON` 的未签名离线镜像 `0xd2bb0` B，SHA-256 `e7a7cfd78f9d3e550f589aae9d4e3e69843011fdbab9395901820c876fb8a9a0` | `0x10fff4` B，SHA-256 `a38417eabd98d7367aabd67b30c0b66b149ae6f09bc0829ad05c46a3aac84936`；双 `0x120000` 槽各余 `0x1000c` |

两份签名构建在各自无 Git 的仓外源码副本、全新 build 目录中完成。C3 使用已记录的 scratch 候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`、只在副本中按候选几何修改的 OTA policy SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`、签名 sdkconfig SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2` 与 RSA v2 测试键 SHA-256 `0dab19dd6a1ebe8ee5a2d634a68e7724bf9d44b827264413988dca1ffb0f6179`。ESP32 使用既有 scratch 候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`、签名 sdkconfig SHA-256 `6ddf140b2845f4e1d4d9bc69b4aad06f446715d7b3d41bfe164b1846a3e0d456` 与 ECDSA v1 测试键 SHA-256 `d6c9a9640b41ab421c225fb55ba44d5198ce7c647e64e96bd743cb4470637e09`；`frp_scratch@0x3ea000/0x10000` 与六页 `base_store@0x3fa000/0x6000` 匹配配置。两个签名 sdkconfig 均启用测试产品 ID `esp-base-capacity-test`。

官方 `check_sizes.py` 用本轮真实构建分区表检查双 `0x120000` 槽容量；`espsecure v5.4.0 verify-signature` 验证 C3 RSA v2 app、ESP32 ECDSA v1 app 与 ESP32 签名分区表，均通过。官方分区表解码核对双槽、产品包区与各自 scratch、`base_store` 几何；两份构建分区表 SHA-256 为 C3 `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25`、ESP32 `49f7ee4e5b70b3bd12cb68e3e10a8ab6d0ac180c24cfa4951b8326c28e2fd602`。

ESP32 初次仓外签名构建曾把启用 scratch 的测试 sdkconfig 与**没有** scratch 的正式 CSV 搭配；虽通过静态验签与尺寸门，该输入运行时缺失分区，因此弃用，不作为产品或 FRPS+Flash provider QEMU 的输入。最终表中 ESP32 app 来自重新装配候选 CSV后的全新 `build-scratch/`；正式 CSV SHA-256 `f3f29e52f2ed0ccb3fbb3faf9e3d978d359aa9a958c2b3a6120399af70f11b73` 未修改。上述候选布局尚未用于本轮 QEMU。

本轮原始解析、普通构建、宿主测试、两份签名构建、官方验签、分区解码和镜像保留在 `mac-work-1:/private/tmp/esp-base-frp-phase-union-0af122-20260927/`。有效签名输入位于 `signed-c3/build/` 与 `signed-esp32/build-scratch/`，ESP32 初次不匹配的静态构建位于 `signed-esp32/build/`，不可复用。两份 host 日志 SHA-256 分别为 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`、`e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`。

本轮没有刷写 C3／ESP32、修改正式分区或凭据，也没有复用旧 ECS2、Flash、QEMU 的 PASS。签名容量与 host 测试不证明 FRPS 会话、MQTT／OTA／guest 并发堆峰值、设备启动、旧 AT／NVS 迁移或 P6-03 五能力验收。
