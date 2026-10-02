# 产品包未决操作冷启动恢复检查点

2026-09-28，Base 将产品安装／升级账本的未决操作检查提前到普通已确认固件启动的 guest 装载之前。启动控制任务先生成本次 `boot_id` 并持有原存储 claim；若最近一条账本记录是 `PREPARED` 安装或升级，按原 operation ID、候选摘要和原 ECS2 序号读取真实签名固件集合与 Container 持久状态。任何记录损坏、身份不符、提交结果不明或独立读回失败都阻断本次启动，保留 claim，不能靠新 boot ID 重放操作。

如果账本意图已提交、Container 尚未预留候选，且原确认绑定可由 `reconcile` 证明，Base 不写 ECS2，直接将原操作持久记为失败。若候选处于 `WRITING`、`PREPARED`、`TRIAL_STARTED` 或 `HEALTH_VERIFIED`，仅在序号、原 operation ID、目标固件与摘要精确对应时调用 Container 的持久放弃入口；随后由 Base 再次独立读回 `ABORTED`、递增序号、原操作和两份不变的固件绑定，再持久结束账本。此前已经 `ABORTED` 且仍能证明原绑定时只读完成账本。`CONFIRMED`、序号越界或其他无法归属的状态保持阻断；本入口不会撤销已提交的产品确认。

这一顺序让损坏的候选包不再抢在原 operation ID 对账之前阻断旧确认 guest。恢复完成后才调用普通 `product_boot` 验签并打开旧包；账本写终态失败时不会启动 guest 或释放启动 claim。无账本键仍走原有的首装资格检查，不据缺键推断历史操作可丢弃。产品专属试运行离线或缺少合格业务事件时仍保持未决；本恢复只处理跨 boot 的未确认候选。

## 验证

- 锁定 `esp-container@d370899b88883d8c23c60884dda9e2dae8bc295d`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 与 wasi-sdk 33，C3／ESP32 两目标真实 RSA 签名 ABI 2 guest 生命周期通过 ASan／UBSan。测试保留 Flash/NVS，模拟新 boot 并破坏候选 Flash；先确认普通 `reconcile` 拒绝损坏候选，再验证错误 ID／旧 boot 不写入、按原 ID 放弃、`ABORTED` 后幂等读回、旧确认 guest 重新验签运行。另验证账本意图提交但候选从未预留时无需改写 ECS2。
- Base 完整 host ASan／UBSan 入口在 C3／ESP32 各 21 项通过。启动假件验证恢复先于 `product_boot`、恢复失败时不打开 guest、不写产品账本终态、不报告 ready；协议假件验证失败保持 `PREPARED`，成功后按原序号记录 `FAILED` 和 Container 结果序号。
- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 下，C3 仓外三份 `0x77000` 包槽候选的正式 USB 测试键签名镜像为 `0x121000` B，SHA-256 `d3c68d3ddc6bca4aa7af9521c5bd7d65b0a99b21ae936746776030093756afee`，官方 RSA v2 验签通过，双 `0x130000` app 各余 `0xf000` B。ESP32 同一源码的显式离线 unsigned probe 构建为 875904 B，SHA-256 `095f442135a6f85906a2db67b269913da9b80f52fe2533f562eebd7afbc0d55a`；它不能用于刷写。
- 仓外 UART 诊断副本使用同一 C3 源码重签为 `0x121000` B，预置当前 app 摘要、真实签名已确认 guest、ECS2 sequence 6 与空操作账本；QEMU 冷启动达到 `ESP_BASE_READY`，`product.status` 回读相同包摘要和 sequence 6。双 app、产品包、FRP scratch、Base 专用 NVS 字节不变；普通 NVS 与 otadata 有启动写入。这验证无未决操作时新增预检不破坏既有启动，不是候选损坏的 QEMU 恢复测试。

构建、签名、宿主与 QEMU 日志及合成 Flash 保存在 `mac-work-1:/private/tmp/esp-base-package-prepare-20260928/`。所有 Flash/NVS 故障注入仍为宿主替身；C3 分区尺寸是尚未迁入正式设备的候选。公开安装／升级入口、受限 HTTPS 来源、业务健康谓词、持久确认、Broker 真消息及实体板掉电均未完成，P6-03／P6-04／P7 不因此验收。
