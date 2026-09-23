# MQTT 实板验收记录

本页是 2026-09-22 旧 Base MQTT 适配层及当时实验镜像的历史验收，不覆盖 2026-09-23 的[公开组件硬切软件候选](mqtt-hard-cut-candidate.md)。

2026-09-22，在迁移基线对应的同一块 ESP32-C3、4 MiB 板卡上，完成官方 MQTT 集成实验应用的一组真实网络与故障测试。P3 仍在实施；普通 ESP Base 尚未接入 MQTT 持久配置、设备命令与 reported。

## 后续：动态订阅、逐轮资源观测与 TCP

同日继续修正并验收动态订阅：新增主题只发送该项，避免重发已有主题触发无关 retained 消息。重新连接仍恢复完整期望列表。订阅/退订提交失败、错误回执、超时均停止会话；正常 stop 按官方实现清空 RAM outbox，尚无最终证据的业务操作不能因此变成成功。ASan/UBSan 覆盖单项/完整 SUBACK 区分、提交失败、UNSUBACK 错配和超时。

在同一 C3 上分别运行两份带资源观测的实验制品（均为 889600 字节）：

- TLS 镜像 SHA-256 `461d5abcef443b5494669dbf20445a5e1b6e2a7701a89bf8d03d8b5b1a494e84`。
- 显式 TCP 实验镜像 SHA-256 `cad339bee1ec3c4240a90cbc7aac33a98e3bbdbf6ee5a2efad1e117736d6dcb7`。构建开关、输入 `.tls=false` / 空 CA 与主机 `--plaintext-lab` 都显式选择明文，TLS 错误不触发切换。

两者均通过十项 QoS/载荷往返和动态订阅、重连恢复、退订、退订后重连不再收取 extra 消息。TLS 的真实 Broker 日志依次证明只订阅 in、只新增 extra、重连订阅 in+extra、退订后重连仅 in。TLS 另通过三次、TCP 通过一次本板 station 暂停五秒后的 MQTT 自动恢复与新消息往返；这不代表外部 AP 断电或 WPA3 验收。

TLS 重新完成 100 次 destroy/create/start；每轮销毁后采样，再取得新 online、新消息和在线资源采样，共 100 个销毁后、101 个在线样本。TCP 另完成 5 次重建及对应 5/6 个样本。

| 新 TLS 逐轮观测 | 结果 |
| --- | --- |
| 销毁后任务 / socket | 每轮 7 / 0；在线每轮 8 / 1 |
| 销毁后 free heap | 207172–207404 字节；首十项中位数 207272，末十项 207368 |
| 在线 free heap | 141360–143204 字节；首十项中位数 141588，末十项 141630 |
| 全程最低 free heap | 103232 字节；诊断制品与首轮未开启 trace 的镜像不能直接比较 |
| 最大连续空闲块 | 始终 114688 字节 |
| 各任务栈最低余量 | main 2060、IDLE 1196、tcpip 2628、esp_timer 3632、wifi 2904、sys_evt 2044、Tmr Svc 1724、mqtt_task 2696 字节 |
| esp_timer | 在线为 19；销毁后 18–19，逐名称核对后续轮次没有增长 |

TCP 的 5 次销毁后也均为 7 个任务、0 个 socket，在线 8/1；各任务栈余量均至少 1196 字节，MQTT 任务为 5140 字节。短时堆采样未观察到持续下降；这不能排除所有资源泄漏，也不代替组合峰值或长稳。

资源工具按初次在线样本核对后续轮次的具名 esp_timer 数量，并单独列出首次初始化差异。TCP 的初始 18 个 ETSTimer 后增加一个 `phy-track-pll-timer`；已核对 ESP-IDF v6.1 的 `components/esp_phy/src/phy_init.c` 与 `phy_common.c`，该对象由 PHY 启停创建/删除。首次分配差异不能直接当作每轮增长；后续增加计时器的派生负例仍被拒绝。socket 扫描并非原子快照，esp_timer 列表不含 FreeRTOS 软件计时器与全部 lwIP 内部超时。

公开 [资源报告工具](../../tools/mqtt_resource_report.py) 验证快照集合、任务/socket 与至少 1 KiB 栈余量，输出计时器和堆趋势；真实日志派生的截断、缺任务、后续增加计时器负例均被拒绝。普通基座再次编译通过，未启用实验 trace/profiling。

本轮保留两类未通过尝试：一次刷写前复合状态断言拒绝，未保存初次失败字段且未写 Flash；复查确认同 UUID、revision 5 与 Wi-Fi 后，改为有界等待并保存每次前置状态。另一份 TCP 输入残留非空 CA，被配置函数以 INVALID_ARG 拒绝，未建立 MQTT 连接；修正私有输入并用同一 C 校验函数验证“旧输入拒绝、新输入通过”后重建，未放宽固件规则。相关原始记录保留在 `p3-mqtt-tcp-20260922/`，不计为 TCP 通过制品。

TLS 与最终 TCP 的每轮刷写都重新核对本轮设备、配置、OTA 选择和双份完整 Flash。两轮结束后均恢复整个原应用槽，两份恢复后完整 Flash 与各自实验前逐字节一致；最后的原 UUID、revision 5、Wi-Fi 与原 Mac Bridge 已恢复，所有临时 Broker 已停止。

新增私有证据分别为 ESP Tool 的 `provisioning/receipts/private/p3-mqtt-lifecycle-20260922/`、`p3-mqtt-tcp-final-20260922/`，含精确源文件快照、构建 receipt、原始日志、网络/资源结果和恢复证明。以下首轮测试继续对应它自己的制品；后续订阅修正没有重跑首轮全部故障矩阵。

## 首轮制品与运行范围

- ESP-IDF v6.1，官方 `espressif/mqtt == 1.1.0`，依赖锁与源码摘要进入私有 build receipt。
- 本机隔离 Mosquitto 2.1.2；控制客户端使用独立 Python 环境与 paho-mqtt 2.1.0。板卡先经 SNTP 同步时间，再以 MQTT 3.1.1 / TLS 1.2、独立实验 CA、主机校验、设备账号与精确 Topic ACL 连接。
- 实板实验镜像为 884016 字节，SHA-256 `5a238ac6ab16504d95cc1071c52340ac9251ae16c9a18ea5eaf7c4607725475a`，含 LAB_ONLY 标记及私有实验输入；它不是公开下载或产品发布制品。此前 884032 字节镜像仅含不可连接的编译 fixture，两者不能混作同一制品。
- 重新核对 USB、身份 NVS CRC、有效 OTA 选择、分区和两份一致的完整 Flash 后，只写既有 `ota_0` 应用槽。实验读取原有 UUID、revision 5 与已提交 Wi-Fi，不修改配置、不驱动 GPIO。

## 已通过的实板项目

| 项目 | 实际验证与结果 |
| --- | --- |
| QoS 与载荷 | QoS 0/1 各验证 0、1、127、1024、4096 字节，逐字节往返一致；1024 字节 SDK 接收缓冲下完成 4 KiB 消息重组 |
| 100 次生命周期 | 逐次 destroy/create/start，等待新 online 并验证新随机消息；串口最终 cycle=100，共 101 次 READY，未发现 panic 或重启 |
| Broker 重启 | 观察断连、新连接与新 SUBACK，再通过全部十项载荷往返 |
| 错误账号 | 临时 Broker 拒绝设备密码，设备报告 AUTH / CONNACK 5，没有 READY；恢复原实验账号后十项往返通过 |
| 错误证书 | 分别使用主机不匹配、非信任 CA、已过期证书；设备均以 X509 验证失败拒绝连接，没有 READY；每项恢复正确证书后十项往返通过 |
| 超限 | 4097 字节触发 FRAGMENT 错误，没有超限回显；随后 4096 字节正常往返 |
| retained 与 stop/start | 保留输入在 clean session 重连、重新订阅后交付；等待设备回显的 PUBACK 后，新宿主订阅者收到 retained 回显；测试保留载荷随后清除 |
| 遗嘱 | 仅在隔离 Broker 以同 ClientID 接管连接，触发真实异常断连；原订阅者收到 offline，新订阅者收到 retained offline，设备自动重连并完成新消息往返 |
| 丢 PUBACK | 在隔离 TLS 测试连接上丢弃 8 个 Broker→设备 PUBACK；观察到 5 次相同 packet ID、相同载荷的 DUP 重传，未实现第二套固件重传器 |
| outbox 满与过期 | 顺序发送八条 4096 字节输入，三条回显占用 outbox 12468 协议字节，另五条返回 OUTBOX_FULL；三条未确认消息分别产生 DELETED / EXPIRED，outbox 归零；恢复 ACK 后新 QoS 1 往返通过 |
| SUBACK 拒绝 | 隔离测试连接将真实 SUBACK 的授权字节改为 0x80，设备报告 SUBSCRIPTION 错误并进入 FAILED，没有 READY；不会自动把拒绝解释成业务在线 |

丢 ACK 与 SUBACK 修改只存在于一次性主机测试连接；两段传输均校验证书，不改生产 Broker 或固件协议实现。PUBACK 仅用于确认协议交付，业务结果仍需独立回执。

## 资源与时延观测

| 指标 | 本轮事实 |
| --- | --- |
| 稳定在线 free heap | 148808–148908 字节，取 state=READY 且 outbox=0 的 30 个样本，覆盖 16 个 cycle 值 |
| 100 次重建全过程最低 free heap | 87620 字节，包含连接过程的瞬时占用 |
| 含后续故障矩阵的最低 free heap | 87184 字节 |
| 稳定在线最大连续空闲块 | 114688 字节 |
| 稳定在线任务数 | 7 |
| 应用 owner 栈最低余量 | 2216 字节；没有测到全部 SDK 内部任务的栈 |
| 十项初始载荷往返时延 | p50 1034.58 ms，nearest-rank p95 1265.47 ms；仅十个样本，不是性能容量结论 |

现有采样未见稳定在线堆持续下降；采样没有覆盖每次释放瞬间，也没有枚举所有设备 socket、计时器和任务栈，不能据此关闭完整资源验收或 72 小时长稳。

## 测试判据修正

第一次证书负例按非零 `esp_tls_cert_verify_flags` 判断，遇到 SDK 已拒绝证书但 flags 为 0。真实串口保留 `mbedtls_ssl_handshake returned -0x2700`。核对 IDF v6.1 的 esp-tls 实现后，以独立 OpenSSL 校验 fixture、设备 X509 错误、故障期间无 READY、恢复有效证书后的完整往返共同验证；没有关闭证书校验或修改 SDK。

第一次 retained 观察者在回显尚未到达 Broker 时已订阅，收到的是实时消息。修正为等待该设备回显的精确 PUBACK 后再建立新订阅者，复测通过。两次原始尝试和修正脚本均保留，不将入队成功当成远端保存证明。

## 恢复与证据

实验后读回完整 Flash，确认应用槽之外的全部字节与实验前一致。恢复整个既有 `ota_0` 槽（包括实验镜像超出原应用长度的部分），随后取得两份完整 Flash，均与本轮实验前基线逐字节相同。再次确认原 UUID、revision 5 与 Wi-Fi connected，原 Mac USB Bridge 已恢复持有端点。临时 Broker 与 TLS 测试代理均已停止。

原始身份、CA/key、账号、配置、固件、Flash 恢复件、串口/Broker 日志、测试脚本、build receipt、资源与恢复结果，仅保存在 ESP Tool 的私有忽略目录 `provisioning/receipts/private/p3-mqtt-hardware-20260922/`；目录 0700、文件 0600。公开主机往返工具为 [mqtt_lab_check.py](../../tools/mqtt_lab_check.py)，使用方式见 [tools README](../../tools/README.md)。

## 尚未关闭的验收

动态订阅/退订、显式 TCP 实验、本板 station 中断恢复和逐轮任务/socket/esp_timer 采样已补齐。外部 AP 中断、WPA3 与更全面的组合资源验收仍待完成；当前采样不包括全部 FreeRTOS 软件计时器和 lwIP 内部超时。P2 的候选/Flash 提交中间态掉电、WPA3 与完整插拔矩阵继续待验。普通基座的 MQTT 配置/命令接入、FRP、OTA、工具网关与 72 小时组合测试属于后续工作；本记录不声明五能力或整个开发计划完成。
