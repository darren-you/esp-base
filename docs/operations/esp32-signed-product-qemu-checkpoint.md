# ESP32 合成 Flash 的签名产品包运行检查点

2026-09-27，在隔离的 `mac-work-1` 上，以 ESP32-D0WD-V3 的仓外条件性签名布局、ECDSA v1 测试签名 Base 和真实 RSA 签名 ABI 2 counter 包制作 4 MiB 合成 Flash。固定 Xtensa QEMU 从该 Flash 冷启动，并用第一次启动后的同片 Flash 再冷启动一次。两轮均由 Base 的正式产品线程完成 Container 包验签、Wasm 初始化，进入 `ESP_BASE_CONTAINER_RUNNING` 和 `READY container=running`。这是仓外预置状态的运行证明；Base 尚无公开产品安装命令。

## 精确输入与离线预置

| 输入 | 固定事实 |
| --- | --- |
| Base | `bdf164750410f7a7f030fa3dbd188ae94c4823c3` 的精确 Git 归档，归档 SHA-256 `bc1f8b2d91becfa8b775d750b159a28c0f2a93d5b1c58757d9901883e525f050`；相对 `26c83d0` 仅增加文档 |
| SDK 与组件 | ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；ESP32 `dependencies.lock` SHA-256 `0593b59a5b8a5e04cfe9eee9262a10e7bc65a220728f026d8097de2f45d534bf`，精确锁定 `esp-container@3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd`，离线 seed 编译使用的六个正式 Container 源文件均与该提交逐文件 SHA-256 相同 |
| 仓外布局与策略 | 条件性 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`、`sdkconfig-esp32` SHA-256 `6ddf140b2845f4e1d4d9bc69b4aad06f446715d7b3d41bfe164b1846a3e0d456`。双 `0x120000` app；`product_pkgs@0x260000/0x186000` 内三槽各 `0x82000`；`frp_scratch@0x3ea000/0x10000`；`base_store@0x3fa000/0x6000`。构建生成的签名分区表经官方解析往返核对，SHA-256 `49f7ee4e5b70b3bd12cb68e3e10a8ab6d0ac180c24cfa4951b8326c28e2fd602` |
| 签名 Base | 仓外 ECDSA v1 测试键；签名 app `0x10fff4`／1,114,100 B，SHA-256 `7196178a5c902f8bfb250e3ea6b26ea42804481f229249e772de03cbf0e918a8`。官方 app／分区签名验证和 `app_check_size` 通过，每个 app 槽余 `0x1000c` |
| 真包与授权 | `product.pkg` 10,240 B，SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`；ABI 2、一页 64 KiB counter Wasm、产品 ID `esp-base-capacity-test`、key ID `capacity-rsa-20260927`。仓外策略 RSA 公钥 DER SHA-256 `ac352916cc880c8ae78273f975647b68db5e58ad63288ec2071b826a7a8c0c55` 与包签名公钥逐字节一致；包的内存、栈、事件、指令与 capability 声明均由正式 validator 按当前策略检查 |

仓外 seed 使用 `esp-container@3b5f16f` 的正式 `slots.c`、`econtainer_package_slot_validate` 和包签名／Wasm 校验实现，只替换 Flash/NVS IO 为测试内存：`initialize → reserve → write_and_prepare → begin_trial → mark_healthy → confirm → reconcile`。它将**刚构建的完整签名 app** SHA-256 `7196178a…` 写作唯一 running／bootable 固件摘要，未沿用旧 C3 Base 摘要。ASan/UBSan 严格执行通过，最终 ECS2 sequence `6`、`CONFIRMED`、slot `0`、blob 288 B／SHA-256 `04767297ce3955606368223a7db83c42e910c6dfb6ecdb8955a80bfc25897388`；包分区前 10,240 B 与原包逐字节相同。仓外 seed 源码 SHA-256 `9023567094435e3cb57cc66eba62478321f4f79d3817cf087d23cce014575ffb`。它仅制造合成前置状态，不是设备安装入口。

固定 IDF 官方 NVS generator 以 V2 blob 格式将 ECS2 写入 `base_pkg/slots`，六页 `base_store` SHA-256 `0eda13b81eb184a3bd0f945a1bc1645fa64073959ac4a793018cbd6fd9599814`；官方 NVS parser 完整性检查通过，第一页 CRC OK、其余五页 Empty。独立合成 Flash 仅填入 bootloader、已验签分区表、初始 otadata、签名 app、包分区和上述 NVS；另一 app、旧 AT 原始归档区及 FRP scratch 初始均为 `0xff`。初始 4 MiB Flash SHA-256 `9f45015b75893a1c8f944ac7d14cc581f3b37f7e28a968049f6ca52098289538`。

## 两次冷启动与持久读回

QEMU 为固定 Espressif Xtensa `esp_develop_9.2.2_20260417`，使用独立 GDB 端口 `12568`；额外堆检查使用 `12569`。两轮各启动一个新 QEMU 进程，第二轮使用首轮结束后同一合成 Flash。两轮 UART 均出现 ECDSA `Verification result 0`、`ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、`ESP_BASE_READY ... container=running`，没有 `BLOCKED`、panic。`RUNNING` 日志在正式 `econtainer_product_init` 返回成功后发出；本检查点未伪造 guest `init` 结果。两轮都有运行态 `free_heap=47,432`、`min_free_heap=43,648` 字节的 reported 样本。

首轮相对初始 Flash 仅系统 `nvs@0x9000` 变化 101 B、`otadata@0x10000` 变化 12 B；首轮后的完整 Flash SHA-256 `7998c62ab0d05d385864ec7236cd7d8894a778e8013afd01d018aaaf61006ca3`，第二轮结束后**整片逐字节相同**。两轮中包分区 SHA-256 始终为 `5addf79b868d63752b81b3ad35e4b911320a4a5f2e557dbbbfe3246ab0a3931b`，`base_store` 始终为上述 `0eda13b8…`，两个 app、旧 AT 归档、scratch 均无变化。两轮读回的 `base_store` 与系统 NVS 均经固定 IDF 官方 parser 完整性检查，活动页 CRC OK；QEMU 专用 eFuse 副本前后 SHA-256 同为 `5f70bf18a086007016e948b04aed3b82103a36bea41755b6cddfaf10ace3c6ef`。

另一次从同一已持久化 Flash 启动的 GDB 检查，在 `ESP_BASE_READY` 日志调用前命中正式 `app_main` 断点。此时 `thread_joinable=true`、`instance_active=true`、启动存储 claim 的 owner/token 均为 0；`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`（`0x804`）空闲 **47,548 B**、最大连续块 **26,624 B**。该时刻与控制任务 reported 样本不同，也不是 FRP、MQTT、OTA 并发峰值。两次冷启动都没有业务事件输入。

原始签名、构建、seed、NVS parser、两轮 UART、逐区 Flash 读回和 GDB 日志保存在 `mac-work-1:/private/tmp/esp-base-bdf-esp32-counter-qemu-20260927/`；测试私钥、产品包、合成 Flash 和含临时生成设备 ID 的 UART 原文均不入 Git。正式 CSV、受控密钥和实体设备未改。此证据不包含公开安装、停止／join／卸载、真实网络、旧 AT 数据迁移、掉电原子性、物理 Flash 时延或五能力同板并发，不能作为实板 P6-03／P7-02 验收。
