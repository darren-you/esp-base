# P6-03 网络回执工作区复用检查点

2026-09-27。从 Base `2f03d7dcc85bb79a6b35e9f7939a3cf6acda6c59` 建立独立 worktree，只合并主控制任务的 JSON 回执与周期 reported 临时缓冲；未修改 FRP、MQTT、凭据、分区或设备。C3／ESP32 仍分别消费 FRP `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`、MQTT `c0677e5e779c3e51e814f2920420be7ec54f1d88` 等原精确锁，两个依赖锁文件的 SHA-256 保持 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`／`5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`。

## 生命周期判定

[`esp_base_protocol.c`](../../firmware/components/device_protocol/esp_base_protocol.c) 原有 1,024 B `s_mqtt_result_json` 与 512 B `s_mqtt_reported_json`。唯一控制任务顺序执行 OTA 完成处理、MQTT 事件轮询、FRP listener、周期 reported 和串口命令；`reply`、`reply_ota_result` 与 `reported` 均在该任务同步格式化。OTA worker 只更新独立请求和原子进度／结果，不写这些 JSON 字节。MQTT 事件回调只填运行层自有槽和通知队列，Base 命令 handler 由控制任务的 `emqtt_poll` 同步调用，不会在 `enqueue` 内重入格式化器。

锁定 MQTT 源码的 `runtime/emqtt.c:414-424` 同步调用官方 `esp_mqtt_client_enqueue`；`mqtt_client.c:2587-2621,2761-2805` 在返回前构包并入队，内置 `lib/mqtt_outbox.c:47-72` 分配自有存储并复制报文及剩余载荷。满额、构包或分配失败均不保存调用方指针。两目标生成配置均为 `# CONFIG_MQTT_CUSTOM_OUTBOX is not set`。因此发送返回后可复用同一 1,024 B 静态工作区。reported 的 `snprintf` 容量和发送上限仍精确保持原 **512 B**，result 继续保持原 **1,024 B**；JSON、Topic、QoS、鉴权和失败返回规则未变。若以后启用自定义 outbox，必须重新核对其复制合同。

## 验证与静态容量

在 `mac-work-1:/private/tmp/esp-base-network-json-audit-20260927/` 用精确 Base Git 归档分别构建基线和修改版；两份源码仅 `esp_base_protocol.c` 与定向测试文件不同。固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 的检查通过；同目标新旧 `sdkconfig` 逐字节相同，C3 SHA-256 `962a52515a5b61e2910e02070f88b4f2ab3063e974a0dac744e516c06f775`，ESP32 为 `46305890efd00df2745c6bb1f694a199e338aaf99683adc5ddbdb0a96522ffdd`。四次普通完整构建通过；ESP32 显式使用仅供离线检查的 `ESP_BASE_ESP32_OFFLINE_PROBE=ON`。

主机测试中，定向协议用例验证 result→reported 后第一条已复制载荷不变、reported→result 后周期载荷不变、reported 超过原 512 B 容量时不发送。C3／ESP32 的完整 ASan/UBSan host 回归分别 **20/20**、**19/19** 通过；原始日志 SHA-256 为 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`／`e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`。两份本地定向协议用例也分别通过 ASan/UBSan。

| 目标 | 旧 `.dram0.bss` | 新 `.dram0.bss` | 旧 `_heap_start` | 新 `_heap_start` | 普通 app 大小前／后 |
| --- | ---: | ---: | ---: | ---: | ---: |
| C3 | `0x15cb0` | `0x15ab0` | `0x3fca9210` | `0x3fca9010` | 913,360／913,360 B |
| ESP32 | `0x16050` | `0x15e50` | `0x3ffca4b0` | `0x3ffca2b0` | 863,168／863,168 B |

两个目标的 `.dram0.bss` 与 `_heap_start` 均精确改善 **`0x200`（512 B）**。旧两符号的链接大小为 `0x400 + 0x200`，新单符号为 `0x400`；`.dram0.data` 不变。C3 旧／新 map SHA-256 为 `c1bec869994614e4300d919c2b121640decbad1ce1b559ff034fc5446c45e029`／`717febe52ed35e69d2ac5ef84f5bff00fe2c0a012c7dc0bd5502880b250d221e`；ESP32 为 `09af42f2904d32123a0eae9a395857013b801cb6d678c05356dc3f28bb66a007`／`0db5d78d765e5b8f1c27778720bb969fc0ed773f23176faed2a83b69dc929ea3`。源码、生成配置、构建日志、host 日志、ELF、map 与 bin 保留在上述仓外路径。

这只证明两目标静态 DRAM 释放与同步发送生命周期，不是 FRPS／Broker／HTTPS 在线峰值、签名 guest 同存或实体板测量。现有 48 KiB 低水和 24 KiB 最大连续块资源门、P6-03 与正式分区仍待完整组合验证；不能把 512 B 机械加到旧 QEMU 读数后宣称通过。
