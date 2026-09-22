# 开发检查点

2026-09-22 后续 P2 实板验收：人工在 RAM 候选配置验证期间断开 USB，收到 running 后的请求第 12.171 秒观察到设备消失，7.482 秒后重新接通。新 boot 报告 power_on，原 UUID、revision 5 与 Wi-Fi 恢复；断电前后各两份完整 Flash 全部逐字节一致，原 Mac Bridge 已恢复。本轮未刷写固件，首次发送前串口 ENXIO 的尝试保留且不计通过；详见[配置候选断电验收](config-power-loss-acceptance.md)。Flash 实际写入中间态掉电仍未完成。

同日后续补齐动态单项订阅、退订与重连期望列表，订阅提交失败改为停止会话。新 TLS 实板完成 100 次逐轮销毁/在线资源采样和三次 station 恢复；显式 TCP 实验通过载荷、动态订阅、一次 station 恢复及五次重建。任务/socket 分别稳定为销毁后 7/0、在线 8/1，所有当前任务栈余量至少 1196 字节。两轮完整 Flash 恢复、UUID、revision 5、Wi-Fi 与原 Bridge 已验证；详见下方验收记录。外部 AP/WPA3、提交中间态断电及五能力组合仍未完成。

2026-09-22 P3 实板网络检查点：同一 C3 完成隔离 TLS Broker 的十项 QoS/载荷往返、100 次完整 MQTT 客户端重建、Broker/认证/证书故障恢复、超限拒绝、retained/LWT、丢 ACK 重传、outbox 满/过期及 SUBACK 拒绝。原固件、完整 Flash、UUID、revision 5、Wi-Fi 与 Mac Bridge 已恢复并验证。详细制品、矩阵、资源范围和失败尝试见 [MQTT 实板验收记录](mqtt-hardware-acceptance.md)。该首轮尚未覆盖的动态订阅、TCP 和逐轮观测已在上述后续检查补齐；普通基座仍未接入 MQTT 设备控制。

以下为同日较早的软件检查点，“未刷写”只描述该时点：

2026-09-22 P3 软件接入检查点：新增 mqtt_runtime 与独立 mqtt_integration 实验应用，锁定官方 `espressif/mqtt == 1.1.0`，没有引入自研 MQTT 报文、QoS 或 outbox。普通基座仍报告 MQTT unsupported；MQTT 尚未接入持久配置、设备命令和 reported。

- 配置、主题与 QoS 边界、4 KiB 有界分片重组、精确 SUBACK 确认、动态订阅/退订、单 owner 生命周期、16 项通知与 3 项完整消息槽已经实现。队列丢失或订阅证明失败会停止会话并明确失败，不能继续声称业务在线。
- SDK 配置检查发现默认未开启证书日期验证；defaults 与编译约束现要求 `CONFIG_MBEDTLS_HAVE_TIME_DATE=y`。TLS 还要求 owner 已同步时间、显式 CA 和主机名校验，实验应用通过官方 SNTP 取得时间。普通应用已实测拒绝实验输入和明文实验选项；正式发布门禁仍待后续实现。
- ASan/UBSan host 回归通过，包含 10000 次分片字段畸变、SDK 初始化/停止失败、outbox 满/过期、错误认证/TLS 分类、订阅拒绝/超时、队列溢出，以及 100 次模拟 SDK 的创建和释放。这些是装配层与事件注入结果，不是板卡/Broker 或真实并发验收。
- ESP-IDF v6.1 / C3 两应用构建通过：普通基座 760576 字节，SHA-256 `a68d33373b2b42a79daa51967d4f85e5a35cbff2fdd840ea4a2debb0aa478b86`；实验应用 884032 字节，SHA-256 `e2f14370768fb3aeacd245dd2f95b6da44c9a91dbe5f1705b4f137c3c6cf2fef`，保留 LAB_ONLY 标记。两者均使用原 4 MiB / 双槽分区。实验输入只是不可连接的示例 fixture，没有真实凭据。
- 构建 receipt、SDK/compiler/CMake/Ninja 版本、源码摘要、依赖锁、sdkconfig、分区、二进制与日志保存在 ESP Tool 私有忽略目录 `provisioning/receipts/private/p3-mqtt-20260922/`。本轮没有刷写设备、连接 Broker 或验证 MQTT 的真实堆峰值。P2 提交期间掉电验收、P3 实板矩阵、正常基座组合与后续阶段继续待完成。

2026-09-22 P2 配置/Wi-Fi 检查点：在同一 C3 上完成 config.set、独立 base_store 分区的完整配置条件提交、候选验证、Wi-Fi RAM 配置与退避重连。当前开发镜像为 760576 字节，SHA-256 `5097fb549ff792680a311144b44340f5470a78bcd11b3694d29e77d07d684773`。

- 错误 SSID、错误口令均未增加 revision，原已提交配置重新连接成功；正确配置、重复请求原结果、同 ID 不同内容冲突、旧 revision 拒绝、显式清除与重新配置均通过。
- 软件重启后同 UUID、新 boot、原 revision 和 Wi-Fi 连接恢复通过。Wi-Fi 在线时 100 次 USB 查询无重启，free heap 范围 211180–211216 字节；这不是五能力峰值或长稳测量。
- Bridge HTTP 配置成功校验新 revision，20 秒失败候选返回明确失败并保留原配置；公开 CLI config.set 通过。最终 revision 为 5。
- 两次完整 Flash 读回摘要一致，核对身份 UUID、NVS header/data CRC、blob 当前索引和完整配置字节；revision 与 Wi-Fi 凭据同 blob 保存，启动区、分区、OTA selector 未改。最终恢复件仅保存在 ESP Tool 私有忽略目录。
- 人工拔除 USB 电源后重新接通，确认真实 power_on、新 boot、同 UUID、revision 5 与 Wi-Fi 自动恢复。首次尝试暴露主机串口库打开端口会额外复位，修复为 POSIX 直接打开并禁用关闭挂断后复测通过；公开 CLI 连续打开三次保持同 boot，伪终端字节传输与背压期限测试通过。
- ASan/UBSan 覆盖命令解析、配置编解码、revision 冲突/耗尽和 SDK 存储调用的故障注入；ESP-IDF 编译通过。上述较早断电发生在配置已提交后；RAM 候选期间断电已在本页后续记录中补齐。Flash 写入中间态掉电、WPA3 和物理插拔完整矩阵尚未验收，P2 保持实施中。

以下 USB 检查点对应本轮较早镜像，保留其范围明确的验收事实。

2026-09-22 P2 USB 检查点：持久身份、每次启动 UUID、严格 JSON Lines 解析、命令 guard、状态结果及重启回执已接入真实 USB。ESP-IDF v6.1 / ESP32-C3 编译与 ASan/UBSan host 回归通过；cJSON 固定为官方 `1.7.19~2`，提交依赖锁。

- 在迁移基线对应的同一块 4 MiB C3 上重新核对 USB、NVS 身份和有效 OTA 选择，取得两份一致的完整 Flash 恢复件；只写入既有 `ota_0` 应用。
- 验证错误设备、旧 boot、过期、超长期限、未知命令、错误参数、重复字段、错误类型、非法 UTF-8、8193 字节超限、分片和半帧超时恢复。连续 100 次状态查询没有重启，free heap 始终为 322036 字节。
- 实板超长输入暴露官方缓冲驱动 RX ring 满后丢数据的问题，改用官方无缓冲 VFS 和 FIFO 背压后整组通过。公开 Python 示例的 status/restart、私有 Bridge 的 HTTP→USB 状态/重启/重复请求/旧 boot 拒绝均通过。
- 写后读回确认启动区、分区表、身份 NVS、OTA selector 和 `base_store` 逐字节未变，应用读回摘要一致；没有修改 eFuse、分区布局或 GPIO。

本轮应用为 160800 字节，SHA-256 `9bfbb1ab10b6dbd0dabed4286c40ebcf075d79bc65422b63ae7409b43d982456`。真实身份、原始日志和恢复字节只进入 ESP Tool 私有忽略目录，不进入公开仓。上述短时 USB 测试不证明配置/Wi-Fi、MQTT、FRP、OTA 或 72 小时长稳；P2 尚未完成。

以下为 2026-09-21 历史基线，当时尚未刷写实板。

2026-09-21 开发基线：ESP-IDF v6.1 / ESP32-C3 构建通过；命令身份、boot、期限、去重与容量 host 回归通过 ASan/UBSan。当前设备协议仍是迁入心跳；命令 guard 尚未接入实际执行，配置/Wi-Fi/MQTT/FRP/OTA 继续实施。未刷写实板。

开发基线已保存至 `master` 的 `16a903e15d2fea2fe5c09a9aa17f622c5821da21`。已从 GitHub 重新 clone 到工作区之外的全新目录进行独立验证，不复用原 checkout 的源码或构建产物。 ESP-IDF v6.1 / ESP32-C3 编译及 ASan/UBSan 命令裁决测试通过；应用大小 0x25120（151840）字节，单槽 0x1e0000，剩余约 92%。

正式发布与完整硬件验收仍未完成。旧独立 checkout 结论只覆盖上述历史 commit，不覆盖后续 USB、配置与 MQTT 改动；当前保存状态以实际 HEAD 与工作树为准。
