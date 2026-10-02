# 产品包内部试运行检查点

2026-09-28，Base 在此前已实现的内部包准备事务之后，新增产品专属的同次启动试运行与放弃入口。上层须持有原产品操作的长事务 claim，并提供持久 `PREPARED` 序号、原 operation ID 和本次 boot ID。旧确认 guest 在准备期间继续运行；只有它经 `stop/close/join` 且 native 实例回收后，候选才能启动。入口先只读预检签名固件集合及精确 ECS2 状态，错误序号或 ID 不写存储、不消耗重开机会。

试运行以已确认固件集合调用 Container 的 `begin_trial`，持久推进 `TRIAL_STARTED`，再重新验签并打开候选包。当前独立 MQTT `event` Topic 的设备端授权与有界队列可把精确包摘要的业务事件交给唯一 guest 线程；旧包摘要被拒绝。`on_event` 返回值只留下易失观察，不能自动把候选确认为持久产品。Base 的固件 OTA 确认入口也明确拒绝产品专属 trial。离线、无合格事件或未定义产品健康谓词时，状态保持未决。

放弃入口先让候选停止、关闭并 join；只有确认 native 实例已回收，才允许 Container 按原 operation、序号与 boot ID 提交 `ABORTED`。Base 随后独立读回序号、操作、候选摘要和两份固件绑定；全部匹配后才开放同次启动旧确认包重开。任何停止、提交或读回不确定都会阻断重开，等待持久状态对账。

## 已验证输入

- 固定 Container `d370899b88883d8c23c60884dda9e2dae8bc295d`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、wasi-sdk 33 下，C3／ESP32 的真实签名 ABI 2 counter 包宿主生命周期均通过 ASan／UBSan。用例验证旧实例运行时拒绝启动、错误序号和 ID 不写入、候选持久进入 `TRIAL_STARTED`、旧包事件拒绝、候选业务事件返回 3、事件后仍未自动确认、放弃读回 `ABORTED` 与旧绑定、同 boot 旧包恢复运行；原 100 次安装／停止／卸载及 native 资源回收回归继续通过。
- 两目标 Base host 回归通过。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 的仓外 C3 三份 `0x77000` 包槽候选，正式 USB 配置重签镜像 `0x121000` B，SHA-256 `ae79f4db56278e73493b5dbe97821b991279da79888f1259c9ec8d1948213a96`；官方 `espsecure` RSA v2 验签通过，双 `0x130000` app 槽各余 `0xf000` B。构建输入和日志保留在 `mac-work-1:/private/tmp/esp-base-package-prepare-20260928/`。
- 同一产品候选在此前 C3 仓外 UART 诊断 QEMU 中以真实签名 guest、ECS2 sequence 6 和独立 NVS 读回验证了已确认包冷启动；该镜像早于本轮试运行入口。它只能证明原绑定启动，不是试运行的设备或仿真端到端证据。

## 尚待闭合

公开 `product.install`／`product.upgrade`、受限 HTTPS 包来源、账本原 ID 终态、产品业务成功与健康谓词、真实 Broker 消息、候选确认及掉电恢复尚未接入。C3 `0x77000` 上限只是候选；正式 C3 分区没有产品包槽，也未写入两块实体板。本检查点不验收 P6-03／P6-04／P7，也不授权刷写设备。
