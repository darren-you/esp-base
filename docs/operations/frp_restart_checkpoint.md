# FRP 认证重启软件检查点

2026-10-01，在 `esp-base@88e4939a31c007849e94d928919710f426d0d730` 的隔离候选上补齐设备端认证 restart。协议事实源为[设备协议](../design/device-protocol.md#frp-base-软件接线边界)；只读状态的原构建证据见[认证状态检查点](frp_authenticated_status_checkpoint.md)。本项不表示私有网关重启、生产 FRPS 或实体板验收完成。

## 行为

listener 由 `frp_status_listener` 硬切为 `frp_management_listener`，CMake、头文件、唯一控制任务、测试入口与活动文档均使用新名，不保留旧 wrapper。仅支持固定 status／restart POST，路径与 JSON 命令必须相符。原四字段只读查询保持；restart 使用普通七字段写合同，复用相同身份、期限、空参数与 SHA-256 指纹逻辑，避免完整配置命令工作区。

FRP restart 与 USB／MQTT 共用 32 槽同 boot 守卫与原 outcome。重复请求只读原状态，不能再次执行或延长原重启时间；冲突、错设备／boot、过期、过长期限和容量耗尽拒绝。OTA pending／运行、产品操作、配置试运行或存储／boot 不确定时拒绝；首次互斥失败同样缓存。认证 running 回执为 HTTP 202、error／result 均 null，解析失败为签名 400，守卫或互斥失败为签名 409；过期保持 expired。

控制任务在回执构造后登记一次 RAM 重启意图，满 100 ms 且已准备响应发送结束／连接关闭才执行；对端停止读取时最多等待 2000 ms。此期间其他通道的新写命令在产品／OTA／配置动作之前返回 operation_busy，原请求回放与只读查询仍可用。序列化失败不登记重启；认证签发或发送失败可能丢失已准入回执，不能判定动作失败。意图与去重表不写 NVS。202、TCP 发送完成与旧 boot 结果都不证明重启成功，控制端必须经同一 FRP 通道核对同一设备的新 boot。

## 软件验证

- C3／ESP32 完整 host ASan／UBSan 各 22 个通过摘要，无跳过；覆盖跨 USB／MQTT／FRP 去重、原 ID 冲突、期限、互斥、容量、序列化失败，以及 100／2000 ms 调度边界。restart 轻量与普通解析器对同一 10000 次确定性变异逐项比对，成功时身份和指纹逐字节一致。
- 显式 `bash firmware/tests/run_frp_management_crypto_tests.sh` 两目标各运行两项，均通过。第一项连接实际 listener、decoder、guard、auth wrapper；Python 独立冻结公开 `00..1f` key 的 status／restart 请求与响应 HMAC，覆盖签名 202／400／409、错端点、错 boot、冲突与篡改。第二项将实际 Base handler、serializer 和重启调度接到同一真实回环 HTTP／HMAC，确认回执先返回、重复请求不重复执行、另一写请求 busy、只触发一次重启。
- 首轮完整测试因新真实解析路径未链接 cJSON／配置校验而停止，随后补齐测试链接；旧测试替身明确禁止同步 restart，扩展为核对既有 100 ms 调用。首轮密码用例错误地把错端点的 400 与成功 HMAC 向量比较，修正断言条件。原失败日志和最终成功日志均保留于仓外，未掩盖设备实现故障。
- 固定 SDK 完整双目标产品测试签名构建、官方 RSA v2／ECDSA v1 验签和应用槽容量通过。SDK 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`，依赖清单／锁、Container／WAMR、分区、配置 schema 与管理密钥均保持原值；复用仓外测试签名键，未使用生产键；两目标实际 ELF 中 auth、restart parser、management listener 与 PSA MAC 四个符号均为已链接 T 符号。

| 目标 | 测试签名 app／槽容量 | 完整 app SHA-256 |
| --- | --- | --- |
| esp32c3 | `0x121000/0x130000` | `6e3e2bd1067756c954ee1689cb7ad94ecb732819394ef206e6759f6a407b3a37` |
| esp32 | `0x10fff4/0x120000` | `e20f2ac2fc2fbecc3088b0206f1f5df2efbdc09b2cf45c2c393457407941759f` |

最终 216 个非 Markdown 输入逐项保存 SHA-256 和执行位，组合指纹为 `12111adfb425e5a8aeae62efa66497010e0957bb565a75def6141c222f7e0d4b`。首次空构建的 215 个输入之后只新增完整回环测试、测试 listener 条件装配与测试编译入口；固件源码／构建配置没有变化，再用最终快照读回签名镜像与槽容量。构建配置、工具链、生成分区、输入清单、原始日志与 receipt 保存于仓外。

## 剩余边界

密码测试的 PSA 端口由 OpenSSL 提供；FreeRTOS、设备重启、持久层和网络 owner 是明确测试替身。没有执行 MCU PSA 运行、真实 TLS／FRPS、五能力并行、堆／栈、两板隔离或长稳验收。私有 Tool 目前只有 FRP 认证只读候选，重启的操作账本、单次写请求和新 boot 确认仍待接入。只有 C3 当前连接，ESP32 已拔除；本轮没有 USB、复位、Flash、NVS、eFuse 或生产发布操作，共享 Root／Profile／依赖指针继续停写。
