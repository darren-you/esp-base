# P6-03：控制栈与协议临时对象容量检查点

2026-09-28。维护者继续使用现有 ESP32-C3 与 ESP32-D0WD-V3 两块 4 MiB 板，普通内部 8BIT 堆历史最低门保持 **49,152 B**。本轮只修改 Base 的经典 ESP32 路径：`base_control` 任务栈由 6144 B 收敛到 4096 B；完整命令联合体、`config.set` 候选配置和提交校验工作区使用 `MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT`。C3 仍使用 6144 B 栈及普通 `malloc`。分配失败沿用现有 `resource_failure` 与候选恢复路径，命令解码、配置持久提交与读回、OTA 串行 owner 均未改。

先前尝试把 ESP32 的 12288 B `base_ota` **任务栈**用固定 SDK `xTaskCreateWithCaps` 放入同一 IRAM 区。正式镜像可以链接，宿主替身测试通过，但仓外 QEMU 运行在 `xTaskCreateStaticPinnedToCore` 的 `xPortcheckValidStackMem` 断言处复位。固定 SDK `578cf89c` 的任务栈检查仍要求 `esp_ptr_byte_accessible`；该 IRAM 区虽能给当前产品的普通数据对象使用，却不能通过栈检查。此尝试已从 Base 产品源码撤回，失败 UART SHA-256 `0d3f18d65e566c5d6859b52120d5a3251df24165d9dcbbc006fbbca27fdf10c7` 保存在 `mac-work-1:/private/tmp/esp-base-ota-iram-stack-probe-20260928/network-qemu-uart.log.iram-invalid`。它不构成 OTA HTTPS 容量验证。

仓外隔离诊断镜像保持固定 SDK、五仓精确锁、测试键 ECDSA v1、原 4 MiB 候选分区与 ECS2 sequence 6。每轮依新 app 摘要重新签名验签、生成 ECS2 和 Flash；Container guest 启动到 `RUNNING`，Base 到 `READY`。OpenETH、本机隔离 Broker 与官方 FRPS 下，TLS 严格验签标志为 0，两条并行工作流分别双向逐字节回显 **300001 B**，Broker 投递三条各 **4096 B** QoS1 消息，三条均在工作后交付。探针另有 4096 B `qemu_frps` 栈和测试时钟，不能代替正式 Wi-Fi／SNTP／HMAC owner。

为覆盖控制任务的配置路径，诊断 app 在 pending 清除后向真实控制循环送入 `status`、`ota.result`、`config.set` 与 `ota.start`。`config.set` 从 revision 0 经 RAM 候选、NVS commit 和读回到 revision 1；同一时刻的 `ota.start` 按真实事务互斥返回 `configuration_busy`。以下是同一脚本负载的**独立运行最低值**，运行波动不能当作逐对象精确节省量：

| 仓外诊断变体 | 普通 8BIT 堆历史最低 | 控制栈最低余量 | 结果 |
| --- | ---: | ---: | --- |
| 4096 B 控制栈，命令与两份配置对象在普通堆 | 42,208 B | 1,576 B | 配置提交、双 FRP、三 MQTT 成功；低于门 6,944 B |
| 两份配置对象改放 8BIT IRAM，命令仍在普通堆 | 45,532 B | 1,608 B | 同路径成功；低于门 3,620 B |
| 命令及两份配置对象均放 8BIT IRAM | **47,776 B** | **1,624 B** | 同路径成功；低于门 **1,376 B** |

最终探针 app `0x10fff4` B，SHA-256 `a3fdcc655333a08832639e85648b23ec9cdcd92135c278fa3ab8385b77264c3a`；官方 ECDSA v1 验签与 `0x120000` app 槽容量门通过。ECS2／Flash 种子 SHA-256 分别为 `d3942f1236eaa14a67d2fde646b792596b081bef9b41a256fa328bad1dd79531`／`01e4891c1a47b44b182f6bdfaa196fe4b877af13d8977ac44d40daf2962ad127`。最终 UART SHA-256 `0cbf416de05ddf30fb8845dc74b97fe2cef1ad69713224d040480f154c402c10`，运行结果与完整串口日志均在上述仓外目录。日志没有 IRAM 申请失败、普通堆申请失败、栈断言或 panic；双流 `completed=2`、`failed=0`，`local_sent=local_received=600002`。该探针仍**未过 49,152 B 门**，前次无配置写入的双流切片最低 47,572 B 也未过门。

将同一产品代码重建为正式两目标签名镜像，Base C3／ESP32 宿主 ASan／UBSan 全套通过。固定 Component Manager 锁分别仍为 `9cda22a703432add42dd4d04f7e70294e91de74bd74041d09d4e067fb51ace42`、`8393de8448b57ba91215177c5a24cc1529f0c78fb595cb79bab3d0238a552d31`，受管 MQTT `runtime/emqtt.c` SHA-256 `ffff14b1143b7a458e0eab9838702826e3c8b1995e809d4d1c643ae2120799c8`。C3 RSA v2 app `0x111000` B，每个正式 `0x1e0000` 槽余 `0xcf000` B，SHA-256 `fe482326f11b231e7f5c091c1e979138fb0c449472927769d37c4a46593e332a`；ESP32 ECDSA v1 app `0x10fff4` B，每个正式 `0x120000` 槽余 `0x1000c` B，SHA-256 `85ce30318c5ff935a2a3ef3df5abf09eb58b946b59f79e3d79afba1bd7d0518e`。两份均通过固定 SDK 官方验签和 `check_sizes.py`。正式分区 CSV 仍缺完整三包槽加 FRP scratch，签名构建不能证明完整 Flash 容量。

控制栈的 1624 B 最低余量属于上述命令与网络切片；尚未执行真正的 HTTPS OTA worker 创建、下载、选择与回滚，也未覆盖最大 FRP AEAD 记录、三槽满时第四条 MQTT 在途、预备流、正式 Base 网络 owner、实体板或掉电恢复。P6-03、P7-01、P7-02 继续开放，不执行实体设备写入。
