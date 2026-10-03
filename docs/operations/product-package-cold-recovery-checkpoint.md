# 产品包未决操作冷启动恢复检查点

## 2026-10-04：历史成功 OTA 收据的恢复顺序

修正此前启动顺序缺口：旧 V3 `SUCCEEDED` 收据先对账后来产品候选，摘要损坏时早于原产品账本恢复退出。现保持完整 C/A 签名固件观察、旧确认序号与已确认引用检查；只对严格后来、目标仍为 C、非固件迁移的产品 `WRITE`，在 `PREPARED`／`TRIAL_STARTED`／`HEALTH_VERIFIED` 返回确切 `UNTRUSTED + CANDIDATE_INVALID` 时继续到原账本恢复。读失败和其他不确定不放行，旧 OTA `PREPARED` 收据的精确身份／相位／序号限制不变。

真实启动仍先对账 OTA，再由原协议入口按未决账本原 ID、fingerprint、包 SHA、ECS2 预期序号和不同 boot 对账、放弃及独立读回候选，将同一原操作记为 `FAILED`（结果 1、实际放弃序号）。之后才启动旧确认产品；无包返回 `EMPTY`，有包完整重新验签并运行 guest。原 `SUCCEEDED` 收据不写，候选包不重放、不擦除，原保护引用保持。此改动不新增持久字段、堆申请、任务、队列或策略；新增判断直接消费原 reconcile 结果，不新增验证读取；放弃、读回、账本终态及旧包装载仍按原恢复路径执行，实际容量节省仍为 0。

两目标原公开签名生命周期与完整 host 入口均实际退出 0；单个组合二进制链接原 `app_main`、原协议恢复／账本算法、真实产品／Container／WAMR及 RSA 签名包。九项正例覆盖历史 `NO_PACKAGE`／`REUSE`／`WRITE` 收据与三个完整未确认相位；三十二项反例核对错误 ID／序号／旧 boot、确认引用损坏、旧 PREPARED 收据、固件集合、候选 I/O、NVS I/O、C 摘要、账本包摘要及序号边界。原生产行为红为明确 READY 诊断和 fixture 非零退出 1，编译／夹具准备失败单独保存。ASan/UBSan 覆盖 Base 产品、协议、账本测试单元；原 main TU 与依赖库普通编译，另保留既有 Darwin 非 SAN 百次重开资源回归。Flash/NVS 和固件观察仍为宿主替身，不证明真实断电／设备原子性、完整 native 生命周期、Flash 时延或联合容量。

本轮在固定 ESP-IDF、逐目标配置／产品分区、活动锁及仓外测试授权／签名材料下完成 C3／ESP32 独立完整 SDK 构建及官方 RSA v2／ECDSA v1 验签，实际退出均为 0。签名镜像分别为 1183744／1114100 B，app 槽余量 61440／65548 B；相对本轮父源码，`.flash.text` 增加 176／112 B，其他 alloc 段和 `_heap_start` 保持。`reconcile_selected_ota` 局部 frame 656 B、`recover_pending_package` 局部 frame 928 B 均未变；ESP32 新独立判断 helper 的 frame 为 32 B，C3 内联。局部 frame 不代表整个启动／恢复调用链峰值，构建不代表实板恢复资格；官方成本收据为 `/private/tmp/historical_ota_base_sdk_h74e05xm/official_verification.private.json`，SHA-256 `f2bbcb2b24c1ac3cc0fe29d3b5dfe6ed8cd256a0a59e2fd27cf026a5c6a66ded`。

## 2026-09-28：原恢复接线历史

2026-09-28，Base 将产品安装／升级账本的未决操作检查提前到普通已确认固件启动的 guest 装载之前。启动控制任务先生成本次 `boot_id` 并持有原存储 claim；若最近一条账本记录是 `PREPARED` 安装或升级，按原 operation ID、候选摘要和原 ECS2 序号读取真实签名固件集合与 Container 持久状态。任何记录损坏、身份不符、提交结果不明或独立读回失败都阻断本次启动，保留 claim，不能靠新 boot ID 重放操作。

如果账本意图已提交、Container 尚未预留候选，且原确认绑定可由 `reconcile` 证明，Base 不写 ECS2，直接将原操作持久记为失败。若候选处于 `WRITING`、`PREPARED`、`TRIAL_STARTED` 或 `HEALTH_VERIFIED`，仅在序号、原 operation ID、目标固件与摘要精确对应时调用 Container 的持久放弃入口；随后由 Base 再次独立读回 `ABORTED`、递增序号、原操作和两份不变的固件绑定，再持久结束账本。此前已经 `ABORTED` 且仍能证明原绑定时只读完成账本。`CONFIRMED`、序号越界或其他无法归属的状态保持阻断；本入口不会撤销已提交的产品确认。

这一顺序让损坏的候选包不再抢在原 operation ID 对账之前阻断旧确认 guest。恢复完成后才调用普通 `product_boot` 验签并打开旧包；账本写终态失败时不会启动 guest 或释放启动 claim。无账本键仍走原有的首装资格检查，不据缺键推断历史操作可丢弃。产品专属试运行离线或缺少合格业务事件时仍保持未决；本恢复只处理跨 boot 的未确认候选。

## 验证

- 锁定 `esp-container@d370899b88883d8c23c60884dda9e2dae8bc295d`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 与 wasi-sdk 33，C3／ESP32 两目标真实 RSA 签名 ABI 2 guest 生命周期通过 ASan／UBSan。测试保留 Flash/NVS，模拟新 boot 并破坏候选 Flash；先确认普通 `reconcile` 拒绝损坏候选，再验证错误 ID／旧 boot 不写入、按原 ID 放弃、`ABORTED` 后幂等读回、旧确认 guest 重新验签运行。另验证账本意图提交但候选从未预留时无需改写 ECS2。
- Base 完整 host ASan／UBSan 入口在 C3／ESP32 各 21 项通过。启动假件验证恢复先于 `product_boot`、恢复失败时不打开 guest、不写产品账本终态、不报告 ready；协议假件验证失败保持 `PREPARED`，成功后按原序号记录 `FAILED` 和 Container 结果序号。
- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 下，C3 仓外三份 `0x77000` 包槽候选的正式 USB 测试键签名镜像为 `0x121000` B，SHA-256 `d3c68d3ddc6bca4aa7af9521c5bd7d65b0a99b21ae936746776030093756afee`，官方 RSA v2 验签通过，双 `0x130000` app 各余 `0xf000` B。ESP32 同一源码的显式离线 unsigned probe 构建为 875904 B，SHA-256 `095f442135a6f85906a2db67b269913da9b80f52fe2533f562eebd7afbc0d55a`；它不能用于刷写。
- 仓外 UART 诊断副本使用同一 C3 源码重签为 `0x121000` B，预置当前 app 摘要、真实签名已确认 guest、ECS2 sequence 6 与空操作账本；QEMU 冷启动达到 `ESP_BASE_READY`，`product.status` 回读相同包摘要和 sequence 6。双 app、产品包、FRP scratch、Base 专用 NVS 字节不变；普通 NVS 与 otadata 有启动写入。这验证无未决操作时新增预检不破坏既有启动，不是候选损坏的 QEMU 恢复测试。

构建、签名、宿主与 QEMU 日志及合成 Flash 保存在 `mac-work-1:/private/tmp/esp-base-package-prepare-20260928/`。所有 Flash/NVS 故障注入仍为宿主替身；C3 分区尺寸是尚未迁入正式设备的候选。公开安装／升级入口、受限 HTTPS 来源、业务健康谓词、持久确认、Broker 真消息及实体板掉电均未完成，P6-03／P6-04／P7 不因此验收。
