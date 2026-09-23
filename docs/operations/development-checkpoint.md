# 开发检查点

2026-09-23 MQTT 公开组件硬切软件候选：移除 Base 原有通用运行层与重复 host 用例，实验应用直接消费公开 `esp-mqtt` 的固定 Git 提交。普通与实验应用的 C3 构建、Base host 回归及唯一组件/锁文件核对见[候选记录](mqtt-hard-cut-candidate.md)。原 2026-09-22 MQTT 实板证据只覆盖当时旧镜像；公开组件的独立实板矩阵及 Base 命令 ACK 闭环尚未完成，本候选未刷板。

2026-09-23 保存前软件复核：当前工作树的 `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 通过；固定 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 与 esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d` 在独立临时 build/sdkconfig 下完成 ESP32-C3 普通构建，镜像 786336 字节、SHA-256 `7f0e394d3795be16caddc51e4d49ebba3b47390012dff53ee79fad794bdfdb50`。同一 SDK 的受控签名构建镜像 1052672 字节、SHA-256 `cbccc852affc8d4b9025e7018df8f48c3c8f1f52d289a780ba57e1c31851de4c`；仓外临时 RSA-3072 测试键的签名经 `espsecure` 验证通过。上述两种镜像均未刷板，临时密钥不入仓，真实 HTTPS、签名槽切换与回滚仍待实板验收。

2026-09-23 P2/P5 网络故障与本地确认软件修正：普通 Base 在 Wi-Fi 驱动初始化失败时保留 USB 控制任务，报告 Wi-Fi `failed`，使 pending 槽仍可按身份、配置和控制进展完成本地确认。配置候选不能凭失败网络提交；网络失败不会单独触发固件回滚。关联/IP 证明改为按实际 SSID 长度比较，忽略驱动 AP 记录中终止符后的填充字节。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；新增真实 Wi-Fi 源码的 11 项 SDK 初始化/配置失败注入，以及不同 SSID 拒绝和 AP 记录填充字节差异的事件验证。ESP-IDF v6.1 / ESP32-C3 普通构建通过，镜像 786320 字节、SHA-256 `12c7044e602573aa2861096ca9c4186db5a0e91342ef1c6b6e6c93eb20915db4`，保持原 4 MiB/双应用槽。构建位于临时目录，未刷板、未验证真实无线故障或新槽回滚。

2026-09-23 P5 OTA operation 持久查询软件切片：签名构建新增只读 USB `ota.result`，以新查询的 request_id 和原 operation ID 读取最近一次 `base_store/base_ota/operation` 收据。`ota.start` 在启动 worker 和写目标槽前单 blob 登记设备 ID、operation ID、完整 signed bin 摘要/长度和源/目标槽，commit 后精确读回；同 operation ID 不跨 boot 重新下载。未决前次结果拒绝新 ID 覆盖，可能需要外部恢复处理；新操作只替换已有成功或失败证据的收据。当前槽 VALID、selector 一致及目标槽状态安全仍是启动前置。worker 活跃或目标 pending 返回 running；只有新槽本地自检确认 VALID 且按收据长度读回的运行镜像 SHA-256 相同才 succeeded；明确的 worker 失败、目标 ABORTED/INVALID 且旧槽有效才 failed，其余 unknown。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；新增真实收据源码 host 故障注入覆盖登记、同 ID 去重/冲突、活跃 worker 不误报失败、pending、VALID 加整镜像摘要、回滚、目标槽不安全及 NVS 写前/写后/commit/读回异常。普通 ESP32-C3 与仓外临时测试键签名 ESP32-C3 构建通过；当前普通镜像 `0xbfee0` 字节、签名镜像 `0x101000` 字节，仍在 `0x1e0000` 槽内；临时测试键的签名验证通过。未刷板、未改生产签名材料。
- 回滚目标若是没有 `ota.result` 的较旧固件，该镜像无法读取新收据；工具必须把最终结果记 unknown，不能从 USB 消失或旧槽出现推断成功/失败。host 假件不模拟真实 NVS 掉电原子性、bootloader 状态转换、设备上整镜像 SHA 耗时与 USB 负载；需要同板双槽部署和断流/回滚实测后才能给设备最终结果验收。

2026-09-23 P5 OTA HTTPS 软件链：受控签名构建增加 USB `ota.start`、独立下载任务、官方 `esp_http_client` HTTPS 与 `app_update`、完整 signed bin 的长度和 staged 槽 SHA-256、IDF RSA-3072 签名校验与 A/B 选择。下载前要求当前运行槽和 boot 槽一致且 otadata 为 VALID，目标为另一 OTA 槽，声明长度不超 `0x1e0000`；HTTPS 证书包校验、禁重定向且要求 HTTP 200/Content-Length 精确匹配。`esp_ota_end` 验签、`esp_ota_set_boot_partition` 切槽后核对 selector 和 SDK 回退可行性，异常尝试选回旧槽并读回；恢复不明时输出 `ESP_BASE_OTA_RECOVERY_REQUIRED`，不自动复位。配置候选与下载互斥，pending 自检独立于公网；签名构建的下载要求本次启动时间同步和 Wi-Fi IP。普通未签名构建拒绝 OTA。

- host ASan/UBSan 全套通过；新增实际 worker 源码的 SDK 故障注入，覆盖超槽、目标错误、响应头及首块 EAGAIN、body 断流、不完整、摘要不符、签名拒绝、selector 回退和恢复失败。pending 确认 API 返回错误但持久 otadata 已 VALID 时按真实状态清门，主程序故障测试已更新。
- ESP-IDF v6.1 / ESP32-C3 普通构建通过，镜像 `0xbee10` 字节；使用仓外临时 RSA-3072 测试键的隔离签名构建通过，签名镜像 `0x101000` 字节（含 64 KiB padding 与签名扇区），仍在 `0x1e0000` 槽内；`espsecure verify-signature --version 2` 校验通过。测试键未入库，本轮未刷板、未改 eFuse 或持久 Wi-Fi，也未连接实际 HTTPS 服务器。
- 当前实板是未签名基座，IDF 的软件验签需要运行中的已签名镜像提供公钥；首次迁移必须验证旧 bootloader rollback、备份/恢复基线、签名运行槽及 VALID otadata，并保留真实回退镜像。测试键构建不是生产签名。应用对 HTTP 取头和每次 body read 的 EAGAIN、零字节及成功返回检查 30 秒无进展/5 分钟总期限；IDF 单次调用仍可能在持续慢速滴流中反复读取，因此这些策略值不是严格墙钟上界。真实 HTTPS/TLS、网络中断、错误签名/摘要/target、完整新槽启动/本地确认/回滚、掉电与外部串口 Flash 租约尚未实板验收；跨 boot 软件回执见页首切片，尚无实板验收。

2026-09-23 P5 普通基座时间同步软件切片：新增 `time_runtime`，普通应用从编译期 `CONFIG_ESP_BASE_TIME_SERVER`（默认 `pool.ntp.org`）启动官方 `esp_netif_sntp`。控制任务每秒以零等待读取同步事件，只有本次 boot 收到事件且 Unix 时间不早于 2024-01-01，心跳才报告 `time_ready=true`；仅有一个看似合理的时钟值不算同步。Wi-Fi 断开后保留本次同步事实与运行中的系统时钟，后续 SNTP 事件会更新判断。初始化失败保持未就绪并报告错误，不触发 pending OTA 回滚或关闭 USB 控制；服务器不进入 NVS 或配置协议。

- 全套 host ASan/UBSan 通过；新增故障注入覆盖初始化失败后重试、无事件不就绪、无效同步时间、时钟回退、SDK 状态失效、非阻塞轮询和服务器字符串生命周期。pending 启动测试确认时间初始化失败不阻止本地 30 秒确认。
- ESP-IDF v6.1 / ESP32-C3 在独立临时 build/sdkconfig 中编译通过，默认时间服务器配置生效，普通镜像 779280 字节，仍在原 `0x1e0000` 应用槽内。本轮没有刷板、修改持久 Wi-Fi、连接 NTP 服务器或生成正式发布制品。
- SNTP 成功事件与日历下界只证明本机已按 SDK 路径同步，不提供加密时间认证；真实网络同步、Wi-Fi 重连后的再次校时和故障恢复仍待实板验收。HTTPS OTA 的后续软件接入见页首记录。

2026-09-23 P5 OTA pending 本地确认门：普通基座先读取运行槽，再完成 NVS、身份、已提交配置和 USB/Wi-Fi 控制任务启动检查。仅对 pending 槽要求控制循环 5 秒内完成首轮，在同一启动 30 秒窗口中每秒核对最近进展不超过 5 秒，窗口终点之后最多再等 5 秒取得新一轮进展，再调用 IDF `esp_ota_mark_app_valid_cancel_rollback()`。pending 期间 `config.set` 返回 `ota_verification_pending`，确认成功后恢复；Wi-Fi、Broker 与 FRPS 在线不是确认条件。启动/活性失败调用 IDF 无效回滚重启；无可回退镜像时不强制重启，日志输出 `ESP_BASE_OTA_RECOVERY_REQUIRED`。确认 SDK 返回错误后读回 otadata，按真实 pending/valid/未知状态裁决，不假定失败原子。构建时强制开启 bootloader rollback。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；OTA 假件覆盖 NVS/身份/配置/控制任务失败、控制循环首轮与中途失活、第 29 秒后退出、跨窗口新进展、5 秒活性边界、30 秒窗口、pending 配置写门及确认后恢复、无回退镜像、SDK 返回错误但持久状态已 valid 等情形。假件不模拟真实 bootloader、Flash 掉电、并发任务或 USB/配置提交最长阻塞时间。
- ESP-IDF v6.1 / ESP32-C3 的普通应用在独立临时 build/sdkconfig 中编译通过，镜像 768480 字节，SHA-256 `c630119149bb65f4b00e9bbb6e66d56e271d5773e613fb1f5aa1741d3c254907`，双 `0x1e0000` 应用槽不变。本轮没有刷写设备或创建正式发布制品。
- 5 秒是当前控制循环的活性策略值，合法长阻塞与真实负载尚待实板验证。此子项当时只关闭启动后的本地确认和失败回滚接线；HTTPS 下载、目标/摘要/签名校验的后续软件接入见页首记录。真实新槽启动、无回退镜像的人工恢复路径、与配置/网络能力并行的实板验收仍待实施。维护者暂缓的人工断电用例不计通过。

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
