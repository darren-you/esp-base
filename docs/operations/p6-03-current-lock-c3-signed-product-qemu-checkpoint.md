# P6-03：当前锁 C3 签名产品冷启动检查点

2026-09-28。仓外 `mac-work-1:/private/tmp/esp-base-current-rtc-c3-product-20260928/` 使用 Base `15ebbc7` 运行源码与正式 C3 精确依赖锁；C3 锁 SHA-256 为 `1ef8526ea6e8238d58c5c26bc25f7a23534524b9f8a5df96cb6132596cd87ac3`，包含 FRP `b462c149`、MQTT `c0677e5e`、OTA `d98361f3`、Container `adef78ff`、WAMR `c10736ff`。固定 ESP-IDF `578cf89c`／lwIP `2758df4`。相对同源 ESP32 产品归档，固件源码差异仅目标锁、仓外 C3 测试候选分区表与对应 OTA policy 两个地址／槽大小宏；`sdkconfig` 则使用 C3 测试签名和目标配置。没有加网络直连探针或改变正式 Base 产品调用链。

测试表在 4 MiB 内使用双 `0x120000` app 槽、三包槽 `product_pkgs`、`frp_scratch@0x3e6000/0x10000` 和八页 `base_store@0x3f6000/0x8000`。这不是正式 C3 分区迁移。C3 RSA v2 测试键签名 app **`0x111000` B**、SHA-256 `8f1abb1311e724711e7f1cd993667bf252aee4abb731d278eae68f6f727c9f06`，每个 app 槽余 **61,440 B**；官方签名验证、分区表解码与 app 尺寸门均通过。仓外 `sdkconfig` SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`，分区二进制 SHA-256 `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25`。签名 ABI 2 counter 包对应 ECS2 sequence 6 `CONFIRMED` SHA-256 `8b7ab8ac6312cdf0bee86188ed6a69a95d763b85b315e7bd6e2571c81c844c03`，种子全 Flash SHA-256 `9fd205793df41e55195fea6d9f70e9e725b705cb144ebe3bd03026c4c7e519af`。

C3 QEMU 仍需旧检查点所述的 **仅限模拟器的 ADC2 校准跳过**。在这个边界内，种子首启和从读回 Flash 的二次冷启动均通过 GDB 停在真实产品线程 `ESP_BASE_CONTAINER_RUNNING sequence=6` 以及 Base `READY` 日志调用点，未把 GDB 路径当成原样 UART 运行证据。两次 GDB SHA-256 同为 `2afa5769cfdf6a7308a5d6906eba9905727213c5fb7c751028a99cd5854a5c87`；产品 pthread 栈未用约 **5,140 B**，READY 时普通内部 8BIT 堆空闲／最大块／历史最低为 **72,892／45,056／66,260 B**。这是无 Wi-Fi、FRPS、Broker、OTA 会话的产品启动值。

首启相对种子：系统 NVS 改动 101 B，`otadata` 改动 12 B，读回为 `ota_seq=1`、`ESP_OTA_IMG_VALID=2`、CRC `0x4743989a`；预填 FRP scratch 按启动恢复擦成全 `0xff`。两个 app 槽、产品包区和 `base_store` 均保持逐字节一致，八页 NVS 头与写入项 CRC 通过，仍有 `base_pkg`、`slots` 的 ECS2 记录。首启全 Flash SHA-256 `5628531d6c570eead347bb0a5908d611bba1105911b855b7fe14c4f97b35a729`；二启相对首启**整片 4 MiB 逐字节不变**。

本检查点仅闭合当前锁 C3 的仓外签名产品启动、测试布局容量与产品持久分区读回。完整包安装／卸载、实际 C3 串口和 Wi-Fi、FRPS／Broker／OTA 同存、正式分区迁移与现有实体板仍待测；66,260 B 不能代表联网低水或 P6-03／P7-02 验收。
