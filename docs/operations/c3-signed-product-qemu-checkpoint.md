# C3 合成 Flash 的签名产品包运行检查点

2026-09-27，在隔离的 `mac-work-1` QEMU 中，以仓外测试键签名的 ESP32-C3 Base 和匹配的 ABI 2 产品包完成两次冷启动。两次均运行到 Container 产品线程的 `init` 成功分支及 Base `READY`，启动存储 claim 已释放。产品包是**仓外预置的合成前置状态**；Base 当前没有对外 `product.*` 安装入口，本检查点不证明设备安装链路。

## 精确输入

| 输入 | 固定事实 |
| --- | --- |
| Base 源码 | `d3144b3a7507eaddd308777b863214fa53b11fcb`；构建副本来自其父提交 `30e608febcf7791857f69fc9eb1fddf39bfb822a` 的归档，`firmware/CMakeLists.txt` 已加入相同的 Container 局部 LTO，SHA-256 `02295ea5e6a5edfe7524dfbf73ddf3bd6ad3908d8df2f301606a213927a425b9`；两提交之间其余构建相关源码未变 |
| SDK | ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` |
| 组件锁 | Container `bf52b17a26e51d35a261bf852ac0c9cde76adefc`、FRP `98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `7f316c2a3a71dcae234b046905aee60696a357d3`、WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5` |
| 仓外 C3 策略 | 候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`，sdkconfig SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`；双 `0x120000` app、三份 `0x82000` 包槽、`product_pkgs@0x260000/0x186000`、`base_store@0x3f6000/0x8000` |
| 签名 Base app | 测试 RSA v2，`0x111000`／1,118,208 B，SHA-256 `4ab810aeb3faf545b530a556cfed037cce74666d48fc50d0942f8381a118f0a9`；官方容量门与 `espsecure.py verify-signature --version 2` 通过，双 app 槽各余 `0xf000` |
| 产品包 | Container `bf52b17` 工具 SHA-256 `d0bcf846e168b0a173c1c866b8b98c09f99a471cf5a3c50682fe2901f812ed51`；ABI 2、固定一页 64 KiB 的 counter Wasm SHA-256 `b9422cb4cb72983141988c4a9a59b602026d94729e2362403a04d1723f98a739`；产品 ID `esp-base-capacity-test`、key ID `capacity-rsa-20260927`；策略内 PKCS#1 公钥 DER SHA-256 `ac352916cc880c8ae78273f975647b68db5e58ad63288ec2071b826a7a8c0c55` 与仓外测试私钥衍生公钥逐字节一致；签名 `product.pkg` 10,240 B，SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e` |

仓外 seed 使用**锁定 Container 的正式 `slots.c` 与 `econtainer_package_slot_validate`**，仅以测试内存 IO 承接 Flash／NVS：按 `initialize → reserve → write_and_prepare`（从槽读回整包并验签、Wasm、产品及授权）`→ begin_trial → mark_healthy → confirm → reconcile` 执行。最终 ECS2 为 sequence `6`、`CONFIRMED`、slot `0`，288 B blob SHA-256 `f2636a491a5f72033a480c5a2e05bb66747011d400b13a7718c639e59bbbc683`；slot 0 的包字节与原包逐字节相同。仓外 seed 源码 SHA-256 `2e4071952aef3b913042ab7d6edf5365c9b0282460a3a5c90ac4039b8a35c2b6`。它生成测试状态，不是 Base 产品安装入口。

固定 IDF 官方 NVS generator 以 V2 blob 格式将该 ECS2 blob 写入 `base_pkg/slots`，得到 32,768 B `base_store`，SHA-256 `dc9d2d8b2dc9d010074695a4e44b8d6fde0104adf818cbc7b92e48289f1ebee1`；官方 NVS parser 的完整页检查返回 0，活动页 CRC OK。4 MiB 合成 Flash 只在原已验签无包镜像基础上覆盖上述包分区与 NVS 分区：原镜像 SHA-256 `f3aa27267775980d9ad16bb263deea8e05895411058346c2f8b3a24493026c22`，预置后 SHA-256 `04d7fb760a531fded2d2fa0a5fd60e2ce4206ceef26d2577b2ee3f2addaca06c`。

## QEMU 结果与资源

QEMU 为固定 Espressif RISC-V `esp_develop_9.2.2_20260417`。每次冷启动只在 `adc2_init_code_calibration` 入口将模拟 CPU 的 `pc` 设为返回地址，以越过 QEMU 无法完成的 ADC2 校准；签名 app、bootloader、分区、SDK、eFuse 文件和正式源码均未因此修改。GDB 分别命中 `esp_base_container_product.c:469` 的 `PRODUCT_RUNNING_AFTER_INIT`（只有 `econtainer_product_init` 返回 OK 才可到达）与 `esp_base_main.c:387` 的 `BASE_READY_LINE`；没有命中本地失败断点，到 `READY` 时 `s_boot_storage_claim={owner=0, token=0}`。第二次冷启动使用第一次运行后的同片 Flash，重复命中两处断点。

| 同一镜像的 QEMU 稳定空闲时刻，`MALLOC_CAP_INTERNAL + MALLOC_CAP_8BIT` | 空闲字节 | 最大连续块字节 |
| --- | ---: | ---: |
| 无包 `READY` 对照 | 151,284 | 114,688 |
| 签名包 `RUNNING`，首次与二次冷启动 | 57,056 | 28,672 |
| 首次运行后仅用 GDB 置位停止请求，worker `stop/close` 返回后的**未 join**状态 | 134,664 | 98,304 |

采样使用 GDB 在 idle 任务停机时调用 `heap_caps_get_free_size(0x804)` 和 `heap_caps_get_largest_free_block(0x804)`；无包与有包是不同合成存储状态，差值包含整个产品启动路径，不能解释为纯 WAMR 分配量或负载峰值。停止请求前先经正式 Base API 取得 storage claim。调试器置位 `stop_requested` 后读得 `stop_succeeded=true`、`instance_active=false`、结果 `STOPPED`，证明该 worker 的 `stop/close` 分支走完；但 `thread_joinable=true`，该签名主应用未链接尚无消费者的 `stop_confirmed` 入口，**没有**证明正式 `stop/join/reopen`。调试 claim 随后经 Base API 释放。

首次运行后 `base_store` 与包分区 SHA-256 仍分别为 `dc9d2d8b2dc9d010074695a4e44b8d6fde0104adf818cbc7b92e48289f1ebee1` 与 `5addf79b868d63752b81b3ad35e4b911320a4a5f2e557dbbbfe3246ab0a3931b`；第二次启动后二者仍逐字节一致。第一次启动的完整 Flash SHA-256 `8a9a09631eed20d8f4ec2cb8a04ca669182a76cd663d9f890e23f77e204cf307`，第二次启动后仍为同一摘要；相对预置前的首次变化只在 `nvs@0x9000` 的 101 B 和 `otadata@0xf000` 的 12 B。两次包区、签名 app 与 ECS2 NVS 没有重复写入；eFuse 副本 SHA-256 始终为 `2054600a17c72426ac024ae851e7ea26f9cf612f31140b445ff713ba15ac09c8`。

原始输入、GDB 命令与日志、NVS parser 结果及两轮摘要收据只保存在 `mac-work-1:/private/tmp/esp-base-d3144b3-product-qemu-20260927/`；私钥、`product.pkg` 与合成 Flash 不进入 Git。QEMU 证明了预置包的真实 Base 读取、Container 验签、guest 初始化、重新冷启动读取与静态资源量测；没有产品安装命令、guest 业务事件、完整停止 join／同次重开、FRPS／Broker／OTA 并行、硬件 Flash 时延、掉电或实板证据。正式分区和实体设备均未改，P6-03／P7-02 仍未验收。
