# ESP32 与 C3 签名 guest 产品线程栈与内部堆窄测

2026-09-27，P6-03 仓外诊断检查点。ESP32 沿用 [ESP32 产品资源归因](esp32-product-resource-attribution.md)的**无 FRP 认证探针**签名 QEMU 输入，基于 Base `0babeec8f39f8af528e23ffa97e94bc77ecd2f79`，固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`。ESP32 `dependencies.lock.esp32` SHA-256 为 `477d558a10d50b456f174477a14ce740bac2bd2313a94048fff4710388ebd23f`，`sdkconfig` SHA-256 为 `ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`。C3 使用下述较早整仓但相同产品线程与 Container 槽运行源码的独立签名输入。**两者都不是后续 MQTT Base `3eae866` 的构建或组合资源结论**；未接入真实 FRPS/TLS、MQTT 会话或 OTA 下载，未触碰实体设备和正式分区。

## ESP32：无探针同锁输入

仅在 `mac-work-1:/private/tmp/esp-base-p603-pthread-stack-20260927/firmware` 的副本中，在正式 Base `product_thread`、`open_selected` 与 Container `slot_runtime.c` 增加一次性采样；仓内生产源码、5,504 B 静态 workspace、16 KiB pthread 栈、8 KiB guest 栈限额、依赖锁、分区和包验证行为均未改。两处临时源码的统一 diff 存于同目录 `diagnostic.patch`，SHA-256 `4b642f2446a037b8e876ba8dd2c8715ddc4916ae8ea73663ddb3b9ec32b0647a`。采样在产品 pthread 自身调用 `uxTaskGetStackHighWaterMark(NULL)`，并以 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 调用 `heap_caps_get_free_size`、`heap_caps_get_largest_free_block`、`heap_caps_get_minimum_free_size`。固定 IDF ESP32 的 `StackType_t` 为 `uint8_t`，栈高水返回**自任务创建以来最低未用字节**，无须乘 4；`NULL` 表示当前任务。诊断打印本身会扰动时序和少量栈/堆，故与无采样基线并列，不把其数值当成无插桩精确峰值。

重新构建的 ECDSA v1 app 仍为 `0x10fff4` B，SHA-256 `abb4dfc669b47f5d1f8a40510c6d39b36bea0486096cab6e12f8423fde42e4ed`；官方 `espsecure verify-signature --version 1` 返回 `Signature is valid`。使用原真实 RSA 签名 ABI 2 counter 包，10,240 B，SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`，按新 app 完整摘要重新生成 sequence 6 ECS2 与 NVS V2，然后生成 4 MiB 合成 Flash，SHA-256 `281f3d9e981b3cf2cb459786aa9205db0f13bb2d423e5d0cb56690e9d30b152f`。Xtensa QEMU 到达 `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、`ESP_BASE_READY ... container=running`，之后又观测 11 秒；UART SHA-256 `0dc2873fbe096ac72ae1b1f9afa7a57eab597fdb6305b40e91622c73fb3f9e31`。原始构建日志、官方验签、seed、采样补丁和 UART 均保留在上述仓外目录。

| 产品 pthread 阶段 | 最低未用栈 B | 内部当前 free B | 内部 largest B | 内部 minimum B |
| --- | ---: | ---: | ---: | ---: |
| worker 入口 | 14,916 | 126,216 | 94,208 | 126,216 |
| 调用 `econtainer_product_open` 前 | 11,988 | 125,220 | 94,208 | 121,428 |
| 签名包验包前 | 11,988 | 125,172 | 94,208 | 121,428 |
| 验包后、WAMR open 前 | **10,564** | 125,172 | 94,208 | 119,168 |
| WAMR open 后 | **10,564** | 47,500 | 26,624 | 47,252 |
| Base 产品 open 返回 | **10,564** | 47,548 | 26,624 | 47,252 |
| guest init 前、后及 RUNNING | **10,564** | 47,548 | 26,624 | 43,640 |

配置的产品 pthread 栈为 **16,384 B**，本次经过的验包、WAMR open、init 路径的最大观测占用为 **5,820 B**，最低未用为 **10,564 B**。`10,564` 在验包结束时第一次出现；WAMR open 和 init 没有刷新该线程高水。另一个 `CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES=8192` 是 Wasm guest 栈限额，不能混算为产品 pthread 余量。guest 运行时内部最大连续块为 26,624 B，高于计划 §13.3 的 24 KiB 单块门 2,048 B。

## C3：相同产品入口源码的独立输入

C3 使用现有 [Classic 双目标签名 QEMU 输入](classic-deadline-qemu-checkpoint.md)的 Base `325ee511b6e48e391ec436fe1d35c6f377d34002` 仓外副本，与本轮 ESP32 的**整仓版本不同**。但其 Base `esp_base_container_product.c` 的 SHA-256 同为 `c5daf2f661c1e13b2a85ebf03692099ed6ff58caa3405a924adc55f6c29878bd`，Container `slot_runtime.c` 同为 `4a1699dacace223c92da2b30542fb7b16486e7612d49fb6c285586fb1e68a7a1`，Container/WAMR 精确锁同上；FRP 锁和其他 Base 源码不能视为相同。C3 候选 CSV `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601` 使用双 `0x120000` app、三 `0x82000` 包槽及与之对齐的**仓外** OTA policy，正式分区未更改。`sdkconfig` SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`，C3 `dependencies.lock` SHA-256 `950997224c0cf3a8a8cf3826e83ef458526d0d7efb2c6331b5896adf0592b8ed`。

仅在 `mac-work-1:/private/tmp/esp-base-p603-c3-pthread-stack-20260927/source/firmware` 的副本加上述阶段探针，并把四项读数存入仓外 `volatile` 数组供 GDB 读取；补丁 `diagnostic.patch` SHA-256 为 `2e8a59e78c914db1947fd91e86adbb832f2a357c131b1842a65dc841cc18b0f3`。重新生成的 RSA v2 签名 app `0x111000` B、SHA-256 `e9eb3cea0ff4aa32c8f8035536bdcd61cf65898a057c34f7bd018a48fbeba44b`，官方 `espsecure verify-signature --version 2` 验证第 0 块成功。真实签名 ABI 2 counter 包为 10,240 B、SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`；以新 app 摘要重新生成 sequence 6 ECS2/NVS，合成 Flash SHA-256 `139dd3c1af235cf45b0d9c567c7bba5b6cd8956e1957791afb6e8bba319db13c`。C3 QEMU 的 USB console 不出现在 UART0；GDB 对 QEMU 未模拟的 ADC2 校准只跳过一次，在 Base `ESP_BASE_READY` 行断点读取已完成的产品线程六阶段与 Container 四阶段数组。`measure-gdb.log` SHA-256 `5a1a7824206168faca400686933b5e750777e0693aa5124e9c5c2ab19c8100df`，数组计数分别为 **6/6** 与 **4/4**。独立测试任务在 `READY` 后才创建；读数不含该任务运行成本。

| C3 产品 pthread 阶段 | 最低未用栈 B | 内部当前 free B | 内部 largest B | 内部 minimum B |
| --- | ---: | ---: | ---: | ---: |
| worker 入口 | 15,012 | 131,504 | 98,304 | 131,504 |
| 产品 open 前、签名包验包前 | 11,564 | 130,208／130,112 | 98,304 | 123,568 |
| 验包后、WAMR open 前 | **10,732** | 130,112 | 98,304 | 123,568 |
| WAMR open 后、Base 产品 open 返回 | **10,732** | 52,440／52,536 | 28,672 | 52,440 |
| guest init 前、后及 RUNNING | **10,732** | 52,536 | 28,672 | 45,408 |

C3 同样配置 **16,384 B** 产品 pthread 栈，本次实际路径最大观测占用 **5,652 B**；栈最低未用 **10,732 B** 也在验包后首次出现。内部最低 **45,408 B**，低于 48 KiB 门 **3,744 B**；最大连续块 **28,672 B**。该堆读数受较早 Base、不同 FRP 锁、候选分区和诊断数组影响，不能与 ESP32 数字相减归因。固定 IDF C3 的栈高水同样以字节返回。

## 裁决边界

ESP32 仓外镜像的内部 heap minimum 为 **43,640 B**，与无采样基线 **43,636 B** 相差 4 B；两者都低于 48 KiB＝49,152 B 初始门。即使用本次 ESP32 诊断读数做不成立的最乐观静态加法，`43,640 + 5,504 = 49,144 B`，仍差 **8 B**，没有给 FRP/TLS/MQTT/OTA 留任何空间。两目标实际经过的验包、WAMR open、init 路径的栈高水均大于 5,504 B，但这只证明本次路径；尚未覆盖 timer/event、stop/close、包失败路径及真设备调度下的最坏调用深度。移动 workspace 后编译器栈帧、对齐和峰值亦需重测。因此本次**没有**把 workspace 搬上栈、缩减产品栈或降低资源门，也没有声称 P6-03 通过。下一步须在当前锁的真实组合负载下采样这些路径并验证最低堆与最大连续块；旧锁 QEMU 读数不能替代当前 MQTT Base 或实板验收。
