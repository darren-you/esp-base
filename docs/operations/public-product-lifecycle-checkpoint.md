# 产品本次启动停止与启动软件检查点

2026-10-02，维护者确认停止只对当前启动生效；重启后，普通恢复入口自动运行已确认产品。Base 已采用下述完整软件候选的相同字节。ESP Tool 完整软件消费者已保存；实体停止／启动和五能力资源门仍待验证，不新增持久停止状态。

## 输入与协议

受测源码为 Base `6882effd90aea8ba6358ddff67fadabb28f56550` 完整导出，加已公开 Base `e302e217f617e30749f286992c2b9115d91f6207` 的 MQTT 精确 manifest／官方生成双目标锁，再加入本批停止／启动。MQTT 为 `6443b71db761f4d667503f14108687bad5e6b5ee`；其余 provider 与 SDK 双锁保持。NVS 两份探针锁及 Wi-Fi 配置字节保持。

`product.stop`／`product.start` 使用原 USB／已认证 MQTT 七字段写请求，顶层 `request_id` 是原操作 ID；参数精确为正 uint32 的 `expected_container_sequence` 与已确认包的 `package_sha256`。结果复用现有本 boot 请求守卫／outcome；`product.result` 的停止／启动结果为四字段，`operation_sequence` 为 null。安装／升级／卸载的六字段持久结果保持。完整字段与状态见[设备协议](../design/device-protocol.md#产品停止与新启动)。

停止只有在 native 资源回收、线程 join、活动视图与事件入口关闭后成功。启动重新验签、装载及 init；同一已确认实例已运行时可只读成功。未决 trial、trap、异常／失败停止与不确定存储均不被 start 自动重开。重启沿原恢复入口对账并运行已确认包；本 boot 的停止结果不会成为跨 boot 持久状态。

复用原唯一产品 worker 与 32 个不驱逐的请求槽，不增加平行 RAM 账本。停止／启动 ID 与持久操作 ID 双向互斥，避免 RAM 结果遮蔽持久结果；活跃产品动作按真实类别判断，避免把已结束安装误读为当前 running。两目标 outcomes 数组均保持 2048 B；新增静态指针 4 B，临时不可变请求 40 B，不能把结构账目算成实体容量验收。

## 软件验证

- 两目标完整 host ASan/UBSan、命令严格解析、HMAC／请求守卫／期限、原 ID 查询、ID 双向冲突与并发动作回归通过。
- 精确 Container／WAMR 的真实签名 guest：三次停止／启动及实际事件、关闭／join／native 回收通过；错误 boot、包摘要与序号拒绝。包与摘要不变而改变测试公钥 modulus 后，fresh start 重新验签并拒绝；本 boot 保持阻断，重置 RAM 的新 boot 恢复原 confirmed。ECS2、整包 Flash 与写计数始终保持。真实 trap 与失败 stop 不允许 start。
- C3／ESP32 各 100 次宿主生命周期资源回归通过；该资源切片不能替代 ESP 历史堆、满负荷或全部任务栈。
- CLI 27 项通过：fresh boot 与产品预检、单次原 ID 写、30 秒内只读轮询、部分写入／断串口／结果丢失返回 unknown 不重发，以及四／六字段严格解码。
- 固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／独立 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的双目标完整普通与签名构建通过。ESP32 普通构建为显式未签名离线 probe。签名复用既有私有测试输入，官方 C3 RSA v2／ESP32 ECDSA v1 验签通过。
- 普通镜像 C3 1,054,144 B、ESP32 989,776 B；签名镜像分别 1,183,744 B／1,114,100 B，均在既有 `0x130000`／`0x120000` 槽内。
- 候选自身完整 provider 文件逐字节／执行位核对及 ordinary／signed 实际编译单元核对通过：MQTT 35／7、FRP 40／15、OTA 11／4、Container 25／10。主树合入的 16 份功能源码／测试／文档与候选逐字节相同，四个构建的实际 Base 编译单元和未改边界也独立核对。

## 证据与剩余边界

70 项软件冷归档 SHA-256 为 `a19321f496917751fab482d3fa9239220050b6218c506ca2ed517d232de3ea55`，24,058,022 B；原始输入、回归、签名／普通 SDK ELF／map／bin、provider 编译回执与审计均冻结。归档及全部 70 项大小／摘要由主执行者独立核验，17 份候选特征输入含候选 commit 记录；仅 16 份功能文件合入，记录使用主树共享入口重新追加。补丁摘要 `b0fd82b6594ae427980be4520835fff925423890f62ee4d18ae69bb8b509708d`。私有证据位于 ESP Tool Git 忽略受限路径 `c3-validation-20261002/public-product-lifecycle-software`。

独立 C 审计摘要 `3036dd2b762f93f79fa69e4c70a6ccc46cb2e7806746ad76c63f1e66e1978d6c`；修复反向 ID 遮蔽与旧持久动作误读后无剩余必修项。CLI 独立回执摘要 `cc8ce2896d227f02b196e3e67017ca8c5ac3f762a432bee95316ccc970391b55`。初始旧 MQTT 构建日志保留，最终结果只对应新精确 MQTT 的完整组合。

本批没有实体板／串口、mac-ci-2 或生产访问。Tool 的 Go／Swift／Web／SQLite／OpenAPI 全部实际消费者另行实现；历史结果必须保留原 boot，不能表示新 boot 的当前停止状态。实体停止／启动、外部 Flash 租约、完整合法峰值、48 KiB 历史堆／24 KiB 连续块／全部任务栈、两板、真实断电、72 小时及生产入口继续开放。P6-03 与总门不据本软件检查点通过。

## 2026-10-03 软件消费者续接

ESP Tool 全链路已公开保存为 `579b9556420aab714d0a9e831afa5055f70cfe2d`；Go／SQLite v8、Swift、Web、两份 OpenAPI 与本地 Bridge 消费同一 boot 的停止／启动合同。完整冷复测及包链六项通过，详细计数和证据见[Tool 检查点](../../../esp-tool/docs/operations/product-lifecycle-checkpoint.md)。对应 15 项 Bruno 请求已重新生成并通过一致性检查；Bruno 共享工作树仍含其他任务变更，未将其整仓保存状态计为本批交付。

本续接仅关闭 Tool 软件消费者边界，实体停止／启动、完整容量、两板与生产验收保持开放。FRP 配置去重与事务重载的软件验证见[唯一配置 owner 检查点](frp-canonical-config-owner-checkpoint.md)。

## 2026-10-03 C3 公开入口实体续验

公开 `daf9cd8` 的 USB／认证 MQTT 停止和启动、原 ID 只读重复、停止后新 boot 自动运行与旧 ID unknown 已按维护者裁决实际通过；十二项业务、卸载、签名 A/C 读回及原代码恢复完成。证据与剩余仪器边界见[C3 实体检查点](product-lifecycle-c3-checkpoint.md)。来源历史 heap 23888 B 仍低于 49152 B；有限切片不关闭满合法峰值、百次公开循环、ESP32、断电、72 小时或生产总门。
