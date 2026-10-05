# C3 公开产品停止与启动实体检查点

2026-10-03，使用公开 Base `daf9cd8d46bb22cad7f9cc6656de995a25e4fd89`，精确消费 MQTT `6443b71db761f4d667503f14108687bad5e6b5ee`、OTA `bf11916ab904be4ee9bcdfae213c85336363e96a`、FRP `989cc876d92b815aeb0b6806fb861f0ee2b39a86`、Container `2b93b979b8b0760dcb96b28ac5d13fc52ae547bf` 与 WAMR `74fd95ccbdc417c3816e04f3308eea8a5473ed34`。本轮关闭 C3 公开生命周期的以下功能切片及恢复边界，完整容量和双板总验收仍未通过。

## 实际输入与授权

固定 SDK 父仓 `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与独立 lwIP child `2758df4cd3666b3b2a5b53830148379326425c0d`；父仓只有该 child 的既有差异。公开 329 份源文件、全部 2752 份 managed 输入（五个 provider 为 2512 份）、实际两个构建各 992 编译单元及 native 锁已核。公开源码只有三处已审阅任务／内存观测叠加，另有 task trace header 与既有实验 CA；没有注入管理调用器或改动生命周期、FRP owner、生产信任及容量预算。

| 镜像 | 字节数 | SHA-256 |
| --- | ---: | --- |
| A，`0.2.0-c3-lab-daf9-a` | 1183744 | `4313d1191a47cd079426d2a4487575d7461fff2fdb66fdc4b065587ebb09e1a9` |
| C，`0.2.0-c3-lab-daf9-c` | 1183744 | `0b012d3af6dbe10bea20ce65256b31f53e38d9ff1fb05df7cb267b0b5e1c669c` |

两份 app 完整构建与官方 RSA v2 离线验签通过，均在 `0x130000` 槽内。正式 Wi-Fi 动态 RX/TX 为 32/32、静态 RX/BA 为 6/6；TLS 为 16384/4096 B 动态缓冲。control 栈 8192 B、产品 pthread 栈 16384 B、Wasm 栈 8192 B、guest 线性内存上限 65536 B。复用有效实验 `test-cancel-key`、CA、包和 timer capability 0x4／max timers 1；没有重新生成密钥或证书，这一 timer 产品不是全部合法能力峰值。

数据动作沿用**本任务已确认授权（Root继承续接事实）**：每轮可丢弃 ESP 全部数据，结束仅恢复原三代码区、擦除旧及实验 NVS、不写 eFuse。现场唯一 USB、serial/MAC、Secure Boot 与 Flash Encryption Disabled、4 MiB 均实时核对；两份本轮完整基线逐字节一致，再独立读取三原代码，同时核对 fresh slice 和原代码身份摘要后才擦写。未复用旧基线或恢复件。

## 公开功能切片

依次完成 USB 配置、严格证书校验与 HMAC 的 MQTT、FRP 认证状态及错误 key 拒绝、HTTPS 安装源产品、真实代表事件与持久确认、一次 WRITE 联合 OTA、新固件与目标包持久确认。下载中 130/130 状态样本显示 MQTT／FRP ready；它不证明整个下载无瞬时离线。

- USB stop/start 各写入一次，原 ID 各有两次只读成功结果；认证 MQTT 同样完成 stop/start。四字段本 boot 结果的 operation_sequence 为 null，原持久操作六字段合同保持。
- 两条停止路径均观察至少五秒的新鲜 reported 与 USB 状态；合法认证业务帧只获得传输 PUBACK，accepted/completed 高水未变化，绑定、包摘要、容器序号和持久序号均保持。随后 start 恢复同一 confirmed，真实 guest 查询事件完成并返回非负结果。
- 再用 MQTT stop 一次，随后沿原一次 restart 进入新 boot；同一已确认产品自动运行，旧 stop ID 在新 boot 的 USB 与 MQTT 查询均为 unknown／product_operation_not_found。符合维护者确认的“只停止当前启动”。
- 五个生命周期原 ID 的应用写入计数各精确为一次，重复均查询原 ID，没有 unknown 后重发。再次运行代表事件与十二项计数、暂停、恢复、到期和业务负例，再卸载并回读空绑定。
- A/C Flash 读回与对应签名镜像逐字节一致。成功及失败恢复脚本先前 22 项纯 mock 失败注入通过；本次实体结束实际全擦、分别恢复原 bootloader 21232 B、partition 3072 B、factory 1048576 B，各自与本轮新鲜读取逐字节一致，复位后取得 wifi_down ACK。

最终功能与恢复 passed=true、cleanup_errors=[]。FRP／HTTPS／MQTT fixture 关闭；Root 独立检查无所属活进程、四个实验监听已关闭、串口未占用。没有访问 mac-ci-2、远端 CI、生产或新增发布队列。

原生回收的证据由已签名源码的成功谓词、原 ID 结果与新鲜状态／业务共同构成。公开接口没有独立 native join 或 handle count 仪器，不能仅由 active:null 推定回收；本轮也没有同 boot 的包／ECS2／数据原始 Flash 接口，不声称完成该字节观察。

## 容量仍失败

来源 boot 的下载历史最低 heap 为 **23888 B**，低于 **49152 B**。沿实际 JSON boot 顺序归属的 20 份来源内存样本中，当前 free 最低 37396 B、最大连续块最低 28672 B、control 栈最低 2392 B；连续块和栈只是采样值，未替代完整门。没有将新 boot 的水位混入来源，也没有把旧轮 22040 B 或静态 2736 B 收益相加减后推定本轮收益。

441 个原生任务快照的 expected/captured 与实际序列化唯一任务数全部一致，另有 7 条未归属快照头的任务行保留。13 个已观察名称的最低水位均至少 1024 B；最低 IDLE 为 1196 B，control 为 2392 B；15 个任务返回观测保留。它们不证明全部生命周期、每个任务实例或满合法峰值。1664 B 工作区、任务 trace 及全部实验观测成本均包含在实际数据中，没有加回。

本轮历史低水由 27544 B 降至 23888 B 的观察区间为 uptime 110482–110655 ms、下载 259008–267200 B，离完整 1183744 B 结束仍远。这只定位历史统计更新区间，未归因某次分配或 TLS／RSA；旧轮约 1.1 MiB 的探针窗口不能直接套用。后续小探针需从新事实重新冻结，当前镜像没有该 allocator 观测器。

## 冷证据与剩余工作

完整准备包含 14038 项受摘要清单约束的文件及五个自引用例外（14043 个归档文件），所有源、build、24 份日志与执行位完整保存；归档 160970347 B，SHA-256 `3d9f408e2570d9e79339728c4da2a5b53760ee59e8eebf3eeb82d4989d0481a6`。实际实体目录 176 项证据加索引，归档 19471897 B，SHA-256 `ba6ac1cf319f63206a67e284eda3c9d1954d73e55a35df52b4c6251661df551a`。两归档逐文件内容／大小／执行位与全清单独立核对。

ESP Tool 忽略目录 `provisioning/receipts/private/c3-validation-20261003/public-product-lifecycle-c3` 保存两归档及九项回执／复核／最终任务名与 boot 归属补验；11 项文件索引 SHA-256 `84c375424e292614a02f1a70ab9f9dd06b5038cd31c0ea759328a142de3fa609`，目录 0700、文件 0600。完整基线、身份、凭据与拓扑不进入公开仓。

ESP32 当前未连接；百次公开重装、全部合法并发峰值、完整连续块与全任务栈、Flash 成本、迟到回调与故障矩阵、真实断电、72 小时、Tool 正式会话与生产交付继续开放。P6-03、P6-09 与总门不据本次有限功能切片勾选。软件合同见[公开生命周期检查点](public-product-lifecycle-checkpoint.md)与[FRP 唯一配置 owner](frp-canonical-config-owner-checkpoint.md)。
