# C3 三份 `0x77000` 包槽容量探针

2026-09-28，在 `mac-work-1` 用固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、Base `29db878d8dbaa4772e9b1d7fc3d97f0de9454f6d` 的源码归档和原有仓外测试 RSA-3072 键，验证一组隔离的 C3 4 MiB 分区候选。Base `57b19fc` 只增加前一轮检查点文档；本探针没有写回正式分区表、设备或生产凭据。输入和完整构建日志位于 `mac-work-1:/private/tmp/esp-base-57b19fc-c3-slot77000-probe-20260928/`。

| 分区 | 起点 | 长度 | 说明 |
| --- | ---: | ---: | --- |
| `ota_0` | `0x20000` | `0x130000` | 签名 app A |
| `ota_1` | `0x150000` | `0x130000` | 签名 app C／回退候选 |
| `product_pkgs` | `0x280000` | `0x165000` | 三份各 `0x77000`，起点为 `0x280000`、`0x2f7000`、`0x36e000` |
| `frp_scratch` | `0x3e5000` | `0x10000` | FRP 独立密文 scratch |
| `base_store` | `0x3f5000` | `0xb000` | 11 页 NVS，止于 `0x400000` |

前置系统 NVS、otadata、phy、coredump 仍占 `0x9000..0x20000`。仓外同时修改候选 CSV、C3 OTA policy 的 app 地址／长度，以及 `sdkconfig` 中 FRP、Container 包区／三槽、Base NVS 的精确配置；ESP32 目标分支没有改。ESP-IDF `gen_esp32part.py` 从 CSV 生成二进制并反向解析，分区无重叠且止于 4 MiB。

初次复制的 `sdkconfig` 继承了上一轮尺寸实验关闭的六条额外 TLS 曲线、P-256 NIST 优化及 SDK 错误名查表，所得 `0x111000` 签名 app **不用于本结论**。恢复这些选项到原默认值后重新完整链接、RSA v2 签名及官方 `check_sizes.py` 检查，得到 `esp_base.bin` **`0x121000` B**、SHA-256 `62b783c7ef9cba396881b64e76ab02110aecfa06af57a927c9a974820c9d4cbf`；每个 `0x130000` app 槽余 **`0xf000` B／60 KiB**。官方 `espsecure verify-signature --version 2` 使用测试键验证签名块 0 成功。生成的分区表 SHA-256 为 `b60a99a70dafed4a0b5089dd1ede3d2c5f310f88d72a40831b82c8fe403c0842`。完整结果在 `build-baseline.log`；裁剪配置初次构建另留 `build.log`，不可混用。

Container 通用打包器允许 512 KiB Wasm，现有最大规范签名样包实际为 `0x82000` B，因此这组 `0x77000` 物理槽会拒绝那类包。当前 Base C3 固件配置的 `max_wasm_bytes` 是 131,072 B；Container 设备验包器按此值加 4,096 B manifest 与 8,192 B 额外开销限制包长，当前上界为 `0x23000` B，落在候选槽内。包槽缩小是否成为正式产品上限仍由维护者裁决，不能因当前固件上限较低就宣称通用签名格式的最大包仍受支持。

## 同几何无包启动与主任务栈

另从上述正式 TLS 候选复制仓外源码，仅将 QEMU 看不到的 C3 USB Serial/JTAG 控制台改接 UART0，并在 `app_main` 末尾增加主任务栈最低余量日志；候选分区、Container policy 和 TLS 曲线不变。完整输入、`run_boot.py`、签名镜像、Flash、GDB／UART 原始日志位于 `mac-work-1:/private/tmp/esp-base-57b19fc-c3-slot77000-qemu-20260928/`。QEMU 9.2.2 的 ADC2 校准缺口仍用 GDB 在函数入口设置 PC 为返回地址；没有改写签名 app 字节。

首次保留 C3 默认主任务栈 **3,584 B** 的完整签名镜像，设备已校验 app、识别 `ota_1` 空槽，并持久建立 `firmware_count=1` 的无包 Container；之后在 `app_main` 再次校验签名镜像时报告 `***ERROR*** A stack overflow in task main`，未到 READY。原始失败保留在 `first-overflow-uart.log`。这是一条真实软件运行阻断，不能把此前静态尺寸通过视为启动通过。

仓外把主任务栈升到 **6,144 B** 后，同布局 UART 诊断签名 app 仍为 `0x121000` B，SHA-256 `2b1076f592f4f2edbf8e0771a3e040b2424272e6efe91cf4dcddee04c5220c04`。QEMU 到达 `ESP_BASE_READY ... container=empty`，公开串口 `status` 返回成功，`product.status` 返回操作高水位 0、下一序号 1、Container sequence 1、空包摘要；`app_main` 栈最低剩余 **2,440 B**。首启后相对种子只修改系统 NVS 101 B、otadata 12 B、Base NVS 1,385 B；双 app、三包槽和 FRP scratch 均逐字节不变。`first-boot-receipt.json` 和 `first-uart.log` 保存首启应答与原始输出。

随后以首启留下的**同片 4 MiB Flash**启动新 QEMU 进程，仍到 `container=empty`，设备 ID 不变、boot ID 更新，`product.status` 的高水位 0／下一序号 1／Container sequence 1／空包摘要不变；主任务栈最低剩余 **4,644 B**。二启前后 Flash SHA-256 同为 `64ec831c85d724158df6646b988ad255f43564c97c04dbe231e1bbedd6cd68c4`，逐字节相等。`boot-receipt.json`、`uart.log`、`first-flash.bin` 和 `flash.bin` 保留二启及对照输入；这只证明 QEMU 软件冷启动，不证明真实掉电。

根据该失败，正式 C3 `sdkconfig.defaults.esp32c3` 将主任务栈设为 6,144 B，CMake 对低于此值的产品配置拒绝构建。在正式 USB Serial/JTAG 候选副本重生成配置并完整构建后，6,144 B 配置的 RSA v2 签名 app 仍为 `0x121000` B、SHA-256 `264d6a93bc37d136997e003b8c12ea9ec728aa13a773151c124ba476a251a94a`；官方签名验证成功，双 `0x130000` app 槽各余 `0xf000` B。`build-main-stack.log` 保留正式控制台容量门；仓外 QEMU 的 UART 诊断镜像与该正式镜像分别记账。

当前证据只证明静态容量与同几何无包启动。公开安装／升级调用路径还未完成，新增代码可能再次吃掉 60 KiB；没有验证该候选的签名 guest、真实 HTTPS/MQTT/FRPS 并存、11 页 NVS 的持续负载、分区迁移、物理 Flash 时延或实体板。P6-03、P6-04 和 P7 继续开放，且未获得任何刷板授权。
