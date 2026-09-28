# C3 候选分区的签名业务包冷启动检查点

2026-09-28，在 `mac-work-1:/private/tmp/esp-base-package-prepare-20260928/qemu-c3/` 使用 Base `77fa202999939cecfe8be4a536275fc68e7f02d3`、Container `d370899b88883d8c23c60884dda9e2dae8bc295d`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 和固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，验证签名 C3 app 从合成 Flash 的已确认产品包启动。此轮没有写实体设备，也没有修改仓内正式分区或 OTA 策略。

诊断副本使用双 `0x130000` app、三份 `0x77000` 包槽、`0x10000` 独立 FRP scratch、`0xb000` Base NVS 的候选表。为让 QEMU 的 UART0 接收 JSON，只在仓外副本切换控制台；为与候选表一致，只在该副本将 OTA 目标槽改为 `0x150000`、槽尺寸改为 `0x130000`。首次诊断副本遗漏后一处改动，导致 `eota_observe_slots` 返回 `EOTA_UPDATE_SLOT_UNAVAILABLE`，Base 因签名固件集合不可确定而拒绝启动产品。NVS 的 `base_pkg/slots` 两次读取均成功且长度为 288 字节。修正诊断副本几何后重新构建、签名、预置并重跑；原失败日志保留为 `first-failure-uart.log` 与 `first-failure-flash.bin`。该问题不是正式源码中的槽位读写故障。

修正后的 RSA v2 签名 app 大小为 `0x121000` B，SHA-256 为 `5505d841826120edffe2117a6b408c44197dc8c4241029dc278ccd1febb499ab`；固定 SDK 的 `espsecure verify-signature --version 2` 验证第 0 个 RSA 签名块成功，每个候选 app 槽尚余 `0xf000` B。独立 host seed 使用精确 Container 槽源码、同一 app 完整摘要与 ABI 2 签名 counter 包（10,240 B，SHA-256 `43661b4639eb3a7ae09d9d66b79617f8dbfcdf9a7f7a821cdf0fa1898b6a257c`），生成已确认 ECS2 sequence 6；固定 SDK NVS V2 generator 写入 288 字节槽位 blob 和 910 字节初始操作账本。官方 NVS parser 验证预置页 CRC 和两个键。预置 4 MiB Flash 的 SHA-256 为 `055de596b4aa5ff18bc85963ed0f688430632203a91784cffa55077e2cb80b65`。

Espressif RISC-V QEMU `9.2.2 (esp_develop_9.2.2_20260417)` 仅在未模拟的 ADC2 校准入口通过 GDB 跳过一次，app 与数据字节未因此改动。UART 记录 `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0` 及 `ESP_BASE_READY ... container=running`；`status` 成功，`product.status` 返回 Container sequence 6、上述产品包精确 SHA、操作高水位 0 和下一序号 1。启动后 Flash SHA-256 为 `d34d2eeae77b59dcc5a294688cb9b82193542a8815af6a5879b81c8c25c8013f`。逐区比较表明启动只修改旧 `nvs` 的 101 字节和 `otadata` 的 12 字节；双 app、分区表、产品包区、FRP scratch 与 `base_store` 均逐字节未变。原始签名构建、`seed.log`、`flash-receipt.json`、`boot-receipt.json`、`uart.log`、`gdb.log` 与比较脚本留在仓外目录。

此结果证明当前测试签名候选布局能冷启动并选择已确认真实 guest。三份 `0x77000` 的正式包容量仍待维护者裁决；本轮没有公开安装／升级入口、授权业务 MQTT 事件、真实 Wi-Fi／Broker／FRPS／HTTPS 并发、物理 Flash 时延、实体板迁移或断电恢复。P6-03、P6-04 和 P7 仍未验收。
