# P6-03：ESP32 六个静态 RX 缓冲与 HTTPS OTA 同片续验

2026-09-28。经典 ESP32 的原生成配置为 `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10`、`CONFIG_ESP_WIFI_RX_BA_WIN=6`。本轮将产品 target 默认静态 RX 数设为 **6**，并由 CMake 拒绝数量或 BA 窗口漂移；C3 已有的六缓冲配置不变。固定 SDK Wi-Fi Kconfig 的 AMPDU RX 约束要求静态缓冲数不低于 BA 窗口，本次保持两者一致。除 Base `firmware/CMakeLists.txt` 和 `firmware/sdkconfig.defaults.esp32` 外，仓外 QEMU 副本 `source/firmware` 与[上一轮 HTTPS OTA 诊断](p6-03-ota-https-combination-checkpoint.md)逐文件相同。签名 app 和 ECS2／Flash 随配置变化重新生成；不能把两轮低水差值解释为单个缓冲的精确大小。

仓外独立目录 `mac-work-1:/private/tmp/esp-base-ota-rx6-capacity-20260928/` 从原诊断副本分出，清理旧生成 `sdkconfig` 中静态 RX 字段的旧别名后由固定 SDK 重新生成。实际配置的 `ESP_WIFI_STATIC_RX_BUFFER_NUM` 与本机 ESP32 旧别名均为 **6**，BA 窗口为 **6**。固定 SDK `578cf89c` 完整构建的测试键 ECDSA v1 签名 QEMU app 为 `0x10fff4` B／SHA-256 `cbc77b4fceee3e0fa6032afd38312258e39644cb7626ae57ad684b7996a952dc`，官方验签通过，`0x120000` app 槽余 `0x1000c` B。按新摘要重绑的 ECS2 sequence 6 SHA-256 `75e0a6004cae48fb966385f9ae50b84268f13c37fe49381365a3c8f7ba15e851`；4 MiB Flash 种子 SHA-256 `2c46bce3e81f0cd0f0bc498f5be132debfa7940e0a5eca5b7825429a5bb634cc`。

同片 OpenETH／隔离 HTTPS、Broker 与官方 FRPS 负载再次完成：OTA 公开 `eota_preflight` 和 `eota_prepare` 经严格 HTTPS 收到并准备独立签名 Base app **1,114,100 B**，目标 inactive app 读回与下载对象逐字节一致；FRPS TLS `verify=0`、Pong 3，两条并行工作流各方向 **300001 B** 回显，`completed=2 failed=0`；三条各 **4096 B** QoS1 MQTT 消息完整交付。运行 app_0、产品包区、`base_store`、bootloader、分区表前后逐字节不变，scratch 恢复全 `0xff`；系统 NVS 改 101 B，otadata 改 12 B。原始 UART SHA-256 `7e833547319a3fc85c53e7875a463390907f837532c8866d7efcda6c7e10fe16`，工作流结果 SHA-256 `a77590928e84eb25e6b49bfa789b9a30943e514735366cd27f489078c09cee57`，HTTPS 服务日志 SHA-256 `4738eba0972863b977df24e0a34c25356465de9227c412129b2f9509dcc3d26b`，运行后 Flash SHA-256 `37a9dddef6ec22f80cf6f915f33c3703db82c6f5f99d2285c10ed347d23efcd5`。

| 签名 QEMU 切片 | 静态 RX | 普通内部 8BIT 堆历史最低 | 距 49,152 B 门 |
| --- | ---: | ---: | ---: |
| 上轮独立镜像 | 10 | 26,416 B | 低 22,736 B |
| 本轮独立镜像 | **6** | **30,052 B** | **低 19,100 B** |

同一源码还在仓外**正式 ESP32 产品 CSV** 下重新完整构建，而非把 QEMU 候选分区误作正式配置：测试键 ECDSA v1 app `0x10fff4` B／SHA-256 `cd47f484dba1685de35ed4f3a52353117f84110803d19a54bc7acc7506f99c0c`，正式分区表 SHA-256 `0b22156f31a15128763fae584490a1100f1b0675c0e3f55ec159a62eb73e62c5`；官方工具分别验签 app 与分区表、`check_sizes.py` 确认槽余 `0x1000c` B。构建目录为 `mac-work-1:/private/tmp/esp-base-ota-iram-product-rx6-20260928/`。正式 CSV 仍未闭合完整三包槽与 FRP scratch，因此该构建不等于目标分区冻结。

**容量门继续失败**，且 QEMU OpenETH 没有验证实体 ESP32 Wi-Fi 的吞吐、重传与断线恢复。正式 Base `ota.start` 收据／退役／Container stage／选槽、第四条 MQTT 在途、FRP 预备流、两块实体板和掉电恢复仍待独立完成；不执行实体设备写入。P6-03、P7-01、P7-02 均保持开放，现有两块 4 MiB 板与 49,152 B 门不变。
