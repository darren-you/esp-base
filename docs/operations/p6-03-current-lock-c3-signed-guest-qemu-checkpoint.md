# P6-03 当前五仓精确锁 C3 签名 guest 容量检查点

2026-09-27，在 `mac-work-1` 的仓外合成 4 MiB Flash 上，用**原样** `esp-base@087f9baaa4414a75106103b46249c80b61189564` C3 测试键签名镜像运行真实签名 ABI 2 counter 包。产品线程完成 guest 初始化并报告 `RUNNING`，随后 Base 到达 `READY`；此时内部堆当前 free/largest 为 **52,712/28,672 B**，启动以来最低 free 为 **45,600 B**，低于 P6-03 的 48 KiB＝49,152 B 门 **3,552 B**。本项仅复测**无网络 C3** guest；没有 FRPS 会话、Broker、HTTPS/OTA 下载或实体板运行，P6-03 仍未验收。

## 精确输入与重新绑定

- 固定 SDK 为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。本次直接复用[同提交离线签名容量检查点](p6-03-five-repo-signed-capacity-checkpoint.md)的仓外 `c3/build/`：其锁 SHA-256 为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`，精确固定 FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`、MQTT `c0677e5e779c3e51e814f2920420be7ec54f1d88`、OTA `d98361f348e19e965efd7462277dde0ae13056fa`、Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。未重新编译或修改该应用。
- C3 app 为 RSA v2 签名 `0x111000` B，完整 SHA-256 `64902674ee7786dad20d18bbaf65499df191f77bcc40a578bfc8e448487b34a6`；官方 `espsecure v5.4.0` 对签名块 0 的 RSA 验签成功。仓外候选分区表 SHA-256 `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25`，双 `0x120000` app 槽与三 `0x82000` 包槽的具体几何见前述容量检查点。该表及匹配的 OTA policy 均未进入正式产品配置。
- 真正的 RSA 签名 ABI 2 counter 包为 10,240 B，SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`。测试公钥 DER SHA-256 `ac352916cc880c8ae78273f975647b68db5e58ad63288ec2071b826a7a8c0c55`，字节与当前构建 `sdkconfig` 的产品公钥完全相同。仓外 seed 沿用同一 Container 提交的正式 `slots.c`、包流式验证、密码验证及 Wasm 校验源码；六个关键源文件的 SHA-256 均与本次构建下载组件逐字节相同。seed 以**本次 app 完整字节**计算固件摘要，依次执行 `initialize → reserve → write_and_prepare → begin_trial → mark_healthy → confirm → reconcile`，得到 `CONFIRMED` 的 ECS2 sequence 6、slot 0 和完整包区；这只构造测试前置状态，不是设备产品安装操作。
- 新 ECS2 blob 为 288 B，SHA-256 `69c4c9acc5dfaa5468db77c390bc38d237c7b047be382427e8e8d54b5bd4c94b`；固定 SDK 官方 NVS V2 generator 写出的 `base_store` 为 32 KiB，SHA-256 `8e49841066efba67dc1d3c15b06c668769a9d076d67a25e6a909cb1be4579037`。从当前 `flasher_args.json` 放入 bootloader、分区表、otadata、**这份**签名 app、包区及新 NVS 后，初始 Flash SHA-256 为 `f2465de1d09a4b1647a6ea4d5ce396697044c9ad3e8b7991a3dda637ede6c8d8`。没有复用旧 app 的 ECS2/NVS seed。

## QEMU 读数与停止点

使用 Espressif `qemu-system-riscv32 esp_develop_9.2.2_20260417`、`-M esp32c3`、上述合成 Flash 与测试 eFuse 副本。GDB **仅一次**在 QEMU 未模拟的 `adc2_init_code_calibration` 入口把 PC 设为返回地址；签名 app、bootloader、Flash 及设备源码未因此改写。C3 产品日志走 USB Serial/JTAG，不出现在该 QEMU 的 UART0，UART 文件只显示 ROM 启动；产品和 Base 状态以断点、真实 FreeRTOS 线程变量及堆函数回读为证。QEMU 命令和 GDB 脚本均保存在仓外证据目录。

| 断点 | 运行事实 | `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 当前 free / largest / minimum |
| --- | --- | --- |
| `product_thread` 完成 `report_result` 后 | `s_product.result=3`，即 `ESP_BASE_CONTAINER_RUNNING`；`boot_admitted=true`、`instance_active=true` | **52,712 / 28,672 / 45,600 B** |
| Base `ESP_BASE_READY` 行 | 同一产品状态仍为 `3`，启动 storage claim 的 owner/token 均为 `0` | **52,712 / 28,672 / 45,600 B** |

GDB 用当前签名 ELF 的符号执行 `heap_caps_get_free_size`、`heap_caps_get_largest_free_block` 与 `heap_caps_get_minimum_free_size`，能力掩码为 `2052`；这些同步函数调用可能轻微扰动测量时序，因此记录精确测试条件，不把它外推为联网峰值。最大连续块 28,672 B 高于 24 KiB＝24,576 B 门 4,096 B；**最低 free 未达到 48 KiB 门**。原样镜像没有链接 `uxTaskGetStackHighWaterMark`，本次未改镜像插桩，也未声称取得当前五仓锁的产品线程栈水位。较早整仓版本的 C3 45,408 B 与线程栈读数只作历史对照，不能替换本项 45,600 B。

在 `READY` 断点后停止 QEMU，运行前后 4 MiB Flash 分区逐字节核对：签名 `ota_0`、整个 `product_pkgs`、`frp_scratch`、`base_store` 均 **0 B** 变化；系统 `nvs` 改 101 B，`otadata` 改 12 B，其余区均不变。运行后完整 Flash SHA-256 为 `d54fba0cb2cccc4a2d9002b17e55b26b1966aae45de2297b9e6d9280527c82f7`。这证明本轮包与 ECS2 绑定在此次启动中保留，不代表物理掉电或 OTA 回退验证。

## 原始证据与边界

仓外目录为 `mac-work-1:/private/tmp/esp-base-087f9ba-p603-c3-guest-20260927/`。其中 `seed.log` SHA-256 `a7702f1f84bc24fa8db8ba0fa2520f1c1181ff85fc4517fed8ccefff98ab51d1`，`guest2-gdb.log` 为 `cb4f5b0f6a9634f358a686388d715129367419c52bcf059e3e310b1f879833a4`，逐区与输入/输出摘要 `guest-receipt.json` 为 `1b1d76f705424fd7d406cfca6ef974cb102131f7e0ca0960b5a15b9f5d8f5e97`；`seed.c`、`prepare.py`、`run.py`、`guest2.gdb`、QEMU 命令、完整 Flash 和原始 UART/QEMU 日志同目录保留。前述构建目录中的 `app-signature-verify.log` 是这份 app 的官方验签原文。

本次 OpenETH 虚拟 NIC 虽由 QEMU 命令创建，但没有配置 Wi-Fi、时间、FRP 管理端点或 MQTT/OTA 任务来建立真实外部会话。不能把 `RUNNING/READY` 及 45,600 B 低水投射为 FRPS、Broker、HTTPS 或五能力并发容量，也不能授权实体 C3 首次新布局刷写。正式分区、原设备数据和签名身份均未修改。
