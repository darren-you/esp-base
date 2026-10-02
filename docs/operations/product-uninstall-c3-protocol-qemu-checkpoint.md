# C3 产品卸载串口协议与签名容量检查点

2026-09-28，以公开 `esp-base@29db878d8dbaa4772e9b1d7fc3d97f0de9454f6d` 的 Git 归档（SHA-256 `f26168bf6e62a3ded04728a39095559430c2388be51768216f3bcc07d3980870`）和固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，在 `mac-work-1` 的两个隔离副本分别做正式 USB 控制台尺寸对照、QEMU UART 协议运行。两种输入不混作同一产品候选，没有写入实体 C3。

## 正式控制台的容量阻断

`/private/tmp/esp-base-29db878-c3-control-build-20260928/` 保持 Base 当前 C3 的 USB Serial/JTAG 控制台和完整五仓源码／组件锁。仓外仅提供测试 RSA-3072 签名键、双 `0x120000` app／三 `0x82000` 包槽／FRP scratch／Base NVS 的 4 MiB 候选分区表，并把 C3 OTA policy 的 app 地址与长度改为该候选几何；没有加入诊断代码或改业务协议。官方 SDK 完整链接、RSA v2 签名成功，随后 `app_check_size` **拒绝** `0x121000` B 签名 app：`ota_0@0x20000` 与 `ota_1@0x140000` 各只有 `0x120000` B，均超 `0x1000` B。`build.log` 保留拒绝原文。这是当前 P6-03 三份最大包槽布局的实际软件容量阻断；签名成功本身不能越过 app 槽尺寸门。

仓外再只关闭 TLS 椭圆曲线表中的 secp384r1、secp521r1、secp256k1 和三条 Brainpool 曲线，保留 secp256r1 与 Curve25519。`build-curves.log` 仍得到 `0x121000` B 并在同一尺寸门失败；其未填充镜像 PADDING 起点 `0x1122e8`，距返回较小签名台阶的 `0x110000` 尚约 `0x22e8` B。继续关闭 SDK 错误名查表后仍为 `0x121000` B、PADDING 起点 `0x1103c0`；再关闭 P-256 NIST 优化后签名镜像才降到 `0x111000` B、官方验签和槽尺寸门通过，但未填充内容已达 `0x10ffc8`，距下次签名台阶只余 **56 B**。这组三项容量探针没有进入产品配置，也没有验证真实 FRPS、MQTT、HTTPS 所需曲线、握手时延和错误可观测性；其 56 B 余量不足以容纳尚未实现的公开安装／升级调用链，不能把偶然装槽当作 P6-03 可交付方案。

## 仓外协议诊断输入

`/private/tmp/esp-base-29db878-c3-protocol-qemu-20260928/c3/` 为了让 Espressif QEMU 的 UART0 接收正式 JSON，只在隔离源码副本把 C3 控制台和 VFS 改接 UART。为了容纳上述 app，**仅该诊断**将两个 app 槽各设为 `0x130000`，三个包槽各缩为 `0x74000`，仍在 4 MiB 内；这不满足正式三份 `0x82000` 最大包合同。C3 `base_control` 栈仍是正式值 6,144 B。`prepare.py`、`build-diagnostic.log`、`flash-receipt.json` 与生成配置保存差异和构建输入；测试 RSA v2 签名 app `0x121000` B、SHA-256 `93347c0936e55991ea05ba0caf65852e94fffb0f02208dd8356c0bb5c429b3ba` 经官方验签及该诊断 app 尺寸检查。

独立 host seed 用同一锁的 Container 槽与验包源码、真实签名 ABI 2 counter 包（SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`）和最终 app 完整摘要生成已确认 ECS2 sequence 6；EPRD v1 初始空账本与 ECS2 由官方 NVS generator 装入八页 `base_store`。合成 Flash SHA-256 `ee6746037f3bd7dc72e908a3cce212fac1c18915bf0be90e90637f244b4b89e6`。QEMU `9.2.2 (esp_develop_9.2.2_20260417)` 仅在 `adc2_init_code_calibration` 入口由 GDB 将模拟 PC 设为返回地址，以绕开其未实现的 ADC2 校准；两次 `*-gdb.log` 保留命中，签名 app 字节不因 GDB 改动。

## 正式设备协议结果

`run_protocol.py` 对首次启动的 UART0 发送 `status → product.status → product.uninstall → product.result`。设备达到 `ESP_BASE_READY container=running`，绑定为 sequence 6／预期包摘要；带当前 device/boot/期限、操作序号 1 与原绑定的卸载返回 `succeeded`，按原 `operation_id` 查询得到 `uninstall`、结果码 0、Container sequence 7。以首次运行写出的**同片 Flash**冷启动得到新 boot ID 与 `ESP_BASE_READY container=empty`；旧 ID 结果仍成功，使用新 request ID 重发相同 operation ID 仍返回成功，ECS2 sequence 7 与账本高水位 1 均未推进。控制任务没有栈溢出。两次原始 `*-uart.log`、QEMU／GDB 命令和最终 Flash 均留在上述仓外目录。

官方 NVS parser 对卸载后和冷启动后的活动页都报 CRC OK；独立解码的 288 B ECS2、910 B EPRD 内部 CRC 也正确且两个 boot 字节相同。ECS2 SHA-256 `1a6025e8813ba709ba346f1a41427369b42e95c71d827aa6d9c51b08a0b0e6aa`，EPRD `e16e097aa37e15b457e99106fb6119b2b61e06e1b6b9df9fc4c079253c640c0a`；账本仅有序号 1 的 `uninstall/succeeded`、高水位 1、Container sequence 7。卸载后和冷启动后的 4 MiB Flash 完全相同，SHA-256 `b09d157b214d0a83a347cb720f22f6276ba1e3d2eb1cab081b74750d38c9a457`；相对前置镜像仅系统 NVS、otadata、Base NVS 分别改变 101、12、2,313 B，双 app 与包区逐字节不变。

这证明在仓外 UART 适配和缩小包槽的诊断镜像中，C3 6 KiB 控制栈可以完成当前正式 JSON 命令、持久结果与重复请求合同；它不证明实体 USB Serial/JTAG 通路、正式三大包槽可装入、真实掉电或 FRPS／Broker／HTTPS 同机。P6-03 的当前尺寸阻断与 P6-04/P7 的完整设备验收均继续开放。
