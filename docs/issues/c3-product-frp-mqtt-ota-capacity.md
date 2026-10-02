# C3 产品、FRP、MQTT 与联合 OTA 组合缺口

最新进展：[MQTT 所有权消费](../operations/mqtt_owned_config_consumer_checkpoint.md)和[RTC 配置所有权](../operations/rtc_config_ownership_checkpoint.md)完成两轮联合功能、十二项业务及代码恢复。最新来源 OTA 历史 heap 为 23800 B、采样连续块 28672 B、控制栈余量 2400 B；48 KiB 门和全部合法瞬时峰值仍未通过。各轮输入、失败和索引分别保留，不将独立样本差额全部归因于单项修改。

后续进展：稳定 run_id 已闭合[第三 boot 登录冲突](../operations/c3-five-capability-run-id-checkpoint.md)。[命令内存与控制栈第二轮](../operations/c3-command-memory-checkpoint.md)的联合功能及原代码恢复通过，控制栈最低余量 2400 B；来源 OTA 历史 heap 11404 B／采样连续块 16384 B 仍不足。以下保留原始失败输入和结果，不覆盖旧收据。

2026-10-02，原始 Base、正式分区及现有精确依赖，在同一 C3 上完成来源产品、FRP/MQTT 在线、公开 WRITE 联合 OTA 和目标产品确认；整轮因随后再次重启的 FRP 登录拒绝停止，且实际内存低水明确未达到 48 KiB。两个问题分别记录，不能把登录拒绝归因于内存不足。

## 输入与范围

运行源码与 `b8d695838328b3664f983baeb7dafc992d5f3982` 逐字节一致，沿用[WRITE 检查点](../operations/c3-joint-ota-write-message-counter-checkpoint.md)的来源 A、目标签名 C、counter 与 message-counter 包，及[FRP/MQTT 检查点](../operations/c3-frp-mqtt-management-checkpoint.md)的官方 FRPS 0.71.0、Tool 原有控制库与隔离严格 TLS 网络。仅私有实验 CA、timer 授权／一个定时器与目标版本配置不同；没有修改固件调用方、生产授权或依赖锁。

来源 A app SHA-256 为 `1fd9e100205d9995568a5c1806ad19103657782cc982ce693c6c4d802832e7ff`，目标 C 为 `1540d0f53f96c01bef5e2e171b5fd2525bcfcfcb8165f9e4a80ebf0d75fbdeea`，各为 1183744 B／RSA v2。来源完整空白数据 Flash 写入后全量核对；观察器 SHA-256 为 `5d24b2e558c7541bdb0e7cc9e50973fe419d402d6de378bd17678e65a46a0f6c`。没有重发安装、OTA 或重启命令，也没有扩大固件等待期限。

## 实际推进与失败点

1. 物理 USB 完整配置推进至 revision 3，MQTT 和 FRP ready。经 Tool 控制库核验设备实际 HMAC，错误管理 key 的空 401 拒绝仍通过。
2. 公开安装 counter，代表事件返回 guest result 19，原连续 30 秒健康门后安装 ID 持久成功。随后经 FRP 认证状态证明产品、MQTT 和 FRP 共存。
3. 公开 `ota.start` 仅发送一次，WRITE 下载不同签名 message-counter 包与目标 C。来源 A 下载过程有 67 份 `0 < ota_received_bytes < ota_total_bytes` 的实际状态采样，全部同时报告 MQTT／FRP ready。现有 worker 在 `eota_prepare` 完成后才停止来源 guest，下载阶段没有主动停掉产品或网络 owner。
4. 目标 C 的新 boot 收到真实代表事件，guest result 2；原健康门后，原 OTA ID 持久成功，目标产品确认，经 FRP 再取得认证状态及 MQTT／FRP ready。
5. MQTT restart 仅发布一次并取得 running 回执；第三个 boot、同设备 UUID、revision 3、确认产品重装载与原 OTA ID 的持久成功均读回。随后 90 秒内的 165 份状态均为 MQTT ready、FRP failed，观察器按原判据失败。

该轮只执行上述两个代表事件。第三个 boot 的业务事件、十二项计数检查、公开卸载与最终 A／C ROM 全字节回读没有执行，不能沿用前一轮结果补写为本轮通过。

## 两项独立缺口

| 实际阶段 | free heap（B） | 历史最低 heap（B） |
| --- | ---: | ---: |
| 来源确认产品＋MQTT＋FRP，认证状态 | 47640 | 17368 |
| 来源 A 正在下载 C，67 份采样中的最低值 | 8048 | 6500 |
| C 的目标产品确认后，认证状态 | 41372 | 23892 |
| 第三个 boot 的失败观察区间最低值 | 61532 | 33872 |

每个 boot 的历史低水独立记录，不跨 boot 相加或合并。6,500 B 明显低于 49,152 B 门；这些读数还没有覆盖最大合法 FRP 记录、MQTT 满队列、连续块、任务栈和全部重连峰值，因此资源总门未通过。对照的前一轮未启用 FRP 的成功 WRITE，34 份来源下载采样历史最低为 38,820 B、采样 free 最低为 47,400 B，资源门同样未通过；功能成功没有关闭容量缺口。

第三个 boot 心跳回报 `frp_error=-14`，精确对应库的 `EFRP_LOGIN_REJECTED`；官方 FRPS 同轮日志为 `client_id ... is already online`，不是 `EFRP_NO_MEMORY`。Base 当前 owner 创建实例提交稳定设备 `client_id`，没有向既有 `previous_run_id` 输入移交已验证 run ID。官方 [v0.71.0 ControlManager](https://github.com/fatedier/frp/blob/v0.71.0/server/control.go)对同 user／client ID 的另一在线 run 拒绝准入；同 run 的替换则走既有生命周期移交。该源码与实测错误一致，缺失 run ID 移交是待修复的接入缺口。尚未完整捕捉旧 control 最终退出时刻，不把所有重启问题归为同一原因，也不改变认证失败的关闭策略。

## 证据、恢复与后续

ESP Tool 私有 `c3-validation-20261002/five-capability-write-first-failed` 保存 109 份原始文件与独立索引；索引 SHA-256 为 `d37f2ec4623cfb57d413809bf728771c351d0c757900cc8fb4ffc68a7111d287`。输入、源码／配置摘要、原始状态与网络日志、失败 NVS／otadata 只读采集、观察器失败和清理结果均保留，未将失败轮次改写为成功。

所有独占实验服务已停止；完整擦除实验数据后，仅恢复原 bootloader、partition table 和 factory app，并逐字节核对。旧／实验 NVS 不恢复，原应用 Wi-Fi down 回执与串口释放已确认。没有 eFuse 或生产服务写入。

后续分别验证稳定设备身份下的已鉴权 run ID 移交及完整重启恢复，并按实际内存域／后续申请收敛容量；不能靠延长观察、重发写命令、放宽 TLS／身份或降低 48 KiB 门标记通过。P4-05／P6-03／P6-09 和完整五能力验收继续开放。

## 稳定身份软件修正续进

2026-10-02，Base 已接入公开 FRP 0.2.0 的调用方稳定请求 run ID，并用已有设备 UUID，不增加持久元数据。官方 FRPS 鉴权先于替换的 host 回归、固定 SDK 双目标及 Base owner 回归通过；详情见[接线检查点](../operations/frp-stable-run-id-checkpoint.md)。本页原失败证据保持，实板重测与容量尚未通过。

## 实板修正与容量续进

2026-10-02，公开稳定 run ID 接线在 C3 的一次 WRITE 联合 OTA、一次 MQTT restart 中完成目标与第三 boot 的 FRP 恢复，十二项业务检查、卸载和代码恢复通过。143 份新成功证据索引为 `effd1620aa917549bd660eb246e00b050f48e80fc0747a630be06b80f2da2988`；本页原 109 份失败保持。新来源下载的 88 份采样全部 MQTT／FRP ready，最低本次 free 为 9080 B、最低历史为 4124 B，容量仍失败。具体边界见[联合检查点](../operations/c3-five-capability-run-id-checkpoint.md)。
