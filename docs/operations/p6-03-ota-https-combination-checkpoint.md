# P6-03：HTTPS OTA 与双 FRP／三 MQTT 同片容量检查点

2026-09-28。本检查点继续使用现有 ESP32-D0WD-V3 4 MiB 布局的软件候选和 **49,152 B** 普通内部 8BIT 堆历史最低门。所有写入仅发生在 `mac-work-1:/private/tmp/esp-base-ota-https-capacity-20260928/` 的 QEMU 合成 Flash；没有写实体板。仓外探针源码 SHA-256 为 `11167fb90e9ae15b89bc8b59bb4b38edca134cca9bedd8e7d2a9f5d2f4d71c91`，Base 产品源码取自 `c58e6ebd09e4218576baa942b5d576bd00b7b354`，OTA 使用当前锁 `d98361f348e19e965efd7462277dde0ae13056fa`，固定 SDK 为 `578cf89c`。ESP32 Component Manager 锁 SHA-256 为 `8393de8448b57ba91215177c5a24cc1529f0c78fb595cb79bab3d0238a552d31`。

测试键 ECDSA v1 签名诊断 app 为 **1,114,100 B**（`0x10fff4`），SHA-256 `a31dbbc8319b879a002e2dd78ade305f0e8421da695a7975eccb30966f85e194`；固定 SDK 官方签名验证通过，`check_sizes.py` 给每个 `0x120000` app 槽留下 `0x1000c` B。下载对象是另一次正式 Base 产品构建的完整签名 ESP32 app，同为 **1,114,100 B**，SHA-256 `85ce30318c5ff935a2a3ef3df5abf09eb58b946b59f79e3d79afba1bd7d0518e`，此前已通过官方签名验证。诊断 app 编入该对象的确定长度与摘要，因此下载目标并非自指镜像。仅在仓外 QEMU 注入测试 CA；OTA 实际传输使用库的 `MBEDTLS_SSL_VERIFY_REQUIRED`、证书链、主机名及验签结果检查。

同一 4 MiB 合成 Flash 预装已签名 ABI 2 guest（ECS2 sequence 6），种子 SHA-256 `05dfac6f99cc0bfce52b8effbbcdaa6edcee12a668121cb48c32c5f65c290a82`。Base 到 `READY` 且 Container guest 到 `RUNNING` 后，OpenETH 经严格 TLS 连接官方 FRPS 与本机隔离 MQTT Broker。探针任务持有与正式 FRP scratch 相同的 `esp_base_storage_owner`，以正式 `esp_base_ota_policy(true)` 执行公开 `eota_preflight` 和 `eota_prepare`，另开 12,288 B 普通堆 OTA 任务栈；HTTPS 服务返回独立签名 app。它没有调用 Base 的 `ota.start` 收据、Container stage、`eota_select` 或新槽重启确认。

| 同片结果 | 实测事实 |
| --- | --- |
| OTA 槽预检 | `result=0`，运行／boot 槽 16 为 `VALID`，目标槽 17 初始未跟踪 |
| 严格 HTTPS 下载 | `GET /esp_base.bin` 为 HTTP 200，服务端发出 **1,114,100 B**；`eota_prepare result=0`、收到／准备均为 **1,114,100 B**，owner 已释放 |
| FRP／MQTT | FRPS TLS `verify=0`、Pong 3；两条并行工作流各方向 **300001 B** 逐字节回显，`completed=2 failed=0`；三条各 **4096 B** QoS1 消息完整交付 |
| OTA 任务栈 | 准备完成时 high-water 剩余 **6,592 B／12,288 B**；此数据不覆盖 Base 完整 OTA worker |
| 普通内部 8BIT 堆 | 历史最低 **26,416 B**，比不变门 **低 22,736 B**；记录中没有分配失败或 panic |
| 工作阶段 8BIT IRAM | FRP 工作完成时 free **33,588 B**，最大连续块 **24,576 B**；不能把它直接相加到普通堆门 |

`eota_prepare` 后读回 inactive app `0x140000` 起 **1,114,100 B** 与候选签名 app 逐字节相同；运行 app_0、产品包区 `0x260000/0x186000`、`base_store@0x3fa000/0x6000` 均逐字节不变。独立 scratch `0x3ea000/0x10000` 从预置图案恢复全 `0xff`。系统 NVS 的设备启动身份和 otadata 有预期变化。运行 UART SHA-256 `1b09e96604fdc09e7c1439346d51259755207e896204cc6bc1760a98d2c5196e`，HTTPS 服务日志 SHA-256 `4738eba0972863b977df24e0a34c25356465de9227c412129b2f9509dcc3d26b`，工作流结果 SHA-256 `a77590928e84eb25e6b49bfa789b9a30943e514735366cd27f489078c09cee57`，最终 Flash SHA-256 `17563ef393e7ffe0954c390d8bed2ab415d7b74055551b1b4e5d6fafff71c8d0`。原始构建、脚本、源码、串口、服务端和 Flash 均留在上述仓外目录。

固定 SDK 的诊断 `sdkconfig` 已启用 `CONFIG_MBEDTLS_IRAM_8BIT_MEM_ALLOC=y`，16 KiB TLS 入站与 4 KiB 出站缓冲优先申请 IRAM，不能再把它们当作未处理的普通堆开销。带调试符号的已签名 ELF 经固定 Xtensa GDB 求得 OTA HTTP 传输对象 **424 B**（其中 Mbed TLS context 264 B、config 120 B）；仅迁移这个对象不足以跨过容量门。同轮 FRP ready 时普通堆 free **67,280 B**，OTA 任务刚开始时 **54,888 B**，净差 **12,392 B**，与新建的 12,288 B 普通堆任务栈量级一致；并发任务与 TLS 分配使这不是逐对象精确归因。即使直接收窄该栈，仍须证明 Base 完整收据／退役／stage／选槽路径的栈高水，且无法单凭该处追回 **22,736 B**。

**容量门未通过。** 此切片含额外 4096 B `qemu_frps` 栈、OpenETH 与测试时钟；未走正式 Wi-Fi／SNTP／HMAC 网络 owner，也未覆盖 Base `ota.start` 收据和选择、第四条 MQTT 在途、FRP 预备流与最大认证记录、真实设备时延和掉电恢复。不能用探针与正式产品的容量差额做无证据扣减，也不能凭该次成功下载宣称 P6-03、P7-01 或 P7-02 验收。两块现有 4 MiB 板及容量门保持不变，下一步必须从真实分配路径收敛至少 **22,736 B**，随后复验完整产品调用链。
