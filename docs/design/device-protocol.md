# 设备控制协议 v1

生产命令为 status、restart、config.set、firmware.status、ota.start／ota.result、business.status／pause／resume。原生业务直接编译进固件；动态 product.*、包模式和包摘要字段已删除，旧字段或命令按未知输入拒绝，没有兼容分支。软件与实板资格见[执行计划](../operations/ota-allocation-diagnostic-checkpoint.md)及[软件检查点](../operations/native_software_checkpoint.md)。

## 帧、身份与结果

USB 为 UTF-8 JSON Lines，单帧最多 9,216 B（不含换行）；拒绝 NUL、非法 UTF-8、重复／未知字段、非对象、非整数数值以及超深对象。半帧 2 秒后排空至下一换行。所有 UUID 使用规范小写 UUID v4。

USB／MQTT 的只读 status、firmware.status、business.status 精确包含 `protocol_version:1`、`request_id`、`command`。ota.result 另包含 `parameters:{"operation_id":"<原 UUID>"}`，不携带写期限或目标 boot，允许重启后查询原操作。FRP 只读请求再加入持久 `device_id`，防止查错设备。

写命令精确包含 protocol_version、device_id、target_boot_id、request_id、command、expires_at_uptime_ms、parameters 七字段。boot 必须为当前启动，期限为设备 uptime 的安全整数、晚于执行时刻且不超过 30,000 ms。32 槽本 boot 守卫保存首次指纹与 outcome，不驱逐已用记录；同 ID 相同请求只回放，冲突拒绝。表满拒绝新写，旧结果仍可查询。request_id、operation_id 和查询请求 ID 各自保持真实语义。

响应精确为 protocol_version、device_id、boot_id、request_id、state、error_code、result 七字段；状态包含 running、succeeded、failed、expired、unknown。超时、发送完成和 running 都不等于成功。存储不确定保留原 ID；客户端只能读回，不自动换 ID 或重发写入。

## 固件状态与升级

firmware.status 的 result 精确包含 firmware_sha256、image_size_bytes、target、ota_slot。摘要来自实际运行镜像的完整 signed bin（含签名尾部），在独占 claim 下验签并前后核对 running／boot／槽状态；忙或无法证明时报告 unknown，不使用旧产品状态摘要。C3 target 为 `esp32c3/esp_base`，方案 `esp_secure_boot_v2_rsa3072`；ESP32 为 `esp32/esp_base`，方案 `esp_secure_boot_v1_ecdsa_p256`。

USB／MQTT ota.start parameters 精确六项：operation_id、image_url、sha256、image_size_bytes、target、signature。image_url 是最多 512 B 的 HTTPS URL；sha256 为非零小写 64 位十六进制；长度必须容纳完整签名镜像并适合目标槽；signature 只含精确 scheme。FRP ota.start 精确五项，删除 image_url，固件字节通过随后绑定的入站流提供。两入口复用唯一升级 owner、V4 意图、验签、备用槽和恢复机制。

受理要求签名策略可用、已确认运行 A／boot 一致、准确 inactive 槽、无 pending 或未决收据、无配置试运行／重启。来源 URL／流元数据在任何 app 擦写前静态验证；写前意图持久提交并逐字节读回后才创建 worker／FRP arm。worker 重读原意图，先使旧 inactive 镜像物理不可启动，再分块准备 C；精确长度、完整摘要、芯片／项目及官方签名均通过后才选择新 boot。部分失败只有清理 C 且证明 A 仍 VALID／selected 后才能记录 failed，否则 unknown 并保留占用。

ota.result 的 result 精确五项：operation_id、sha256、image_size_bytes、target、target_slot。原收据与实际槽一致时，活动 worker／pending C 为 running；C 为 VALID、完整身份吻合且成功收据提交读回才 succeeded；A 为 VALID／selected 且原失败收据已完成才 failed。只看 INVALID／ABORTED 或看见 VALID 而缺成功收据都不能推断终态；未登记／被后续操作替换的 ID 为 unknown。普通未签名构建拒绝固件写入与持久结果资格。

新 boot 不依赖 Broker／FRPS 在线确认：本地初始化与控制进展通过完整 30 秒窗口，确认 VALID 并复核后提交成功收据。控制进展跨窗或失活重新累计窗口；确认／读回不确定保持原事实。普通 app OTA 不改变分区表。

V4 为 182 B 固件独立记录。旧 V3、旧长度、损坏、未知或读取失败一律存储不确定；不能自动清空或改成新操作。首次有线迁入审计旧终态、受限归档原收据与物理证据后才在离线候选中退役旧运行键，保留 UUID／配置／revision／凭据。布局／镜像身份替换后旧原 ID 在设备返回 unknown，其历史通过迁入收据核对；新 OTA 的原 ID 持久查询合同保持。未决或损坏迁入阻断，不通过清 NVS 解决。

## 原生业务

business.status result 精确三项：byte_count、state（idle／active／paused）、window_deadline_uptime_ms。business.pause／resume 使用七字段写身份与空 parameters；复用请求守卫。暂停保留计数并清定时窗口，恢复只允许 paused→idle。新启动计数、状态和最近事件重置，不持久化暂停。

MQTT 原始业务字节沿用消息计数样例：首字节 0x01 加至少一个数据字节，按后续字节数累计（含零字节）；0x02 暂停；0x03 恢复；0x04 返回状态数值 0／1／2；0x05 返回计数。除 0x01 外长度必须为 1。idle 的首个计数事件开启 100 ms active 窗口，后续事件不延长，到期变 idle且计数保持。非法输入 -1，暂停拒绝 -2，int32 计数溢出 -3，时间溢出 -4；不新增硬件动作。

业务同步借用当前认证事件内存，无新 FIFO、heap 副本或任务；有限处理完成后记录实际输入 SHA-256 和业务返回。业务负值属于已处理失败，序号推进，避免同一业务拒绝被重复当作未交付。认证／序号／资源准入失败不推进序号。

## MQTT 网络命令合同（固件软件接线候选）

MQTT 3.1.1／严格 TLS 复用既有 CA、主机名、当前 boot 时间门和设备凭据。ClientID 为设备 UUID，精确 command／event 两订阅 SUBACK 均批准后才 ready。命令须精确 Topic、QoS 1、非 retained；载荷为 64 个小写 HMAC-SHA256 十六进制字符、LF 和原始 JSON，认证后才解码。网络 config.set 返回 physical_usb_required。

event 采用 `esp-base-business-event-v1\n` 域隔离。认证原文依次为该 ASCII 域、36 B device UUID、36 B 当前 boot UUID、8 B 大端正 event_sequence 和原生业务字节，外层仍是 64 B tag 与 LF。完整入站帧最多 4,096 B，所以业务字节最多 3,924 B；没有包摘要。序号必须为本 boot 下一值，重连与同 boot 重配不清高水位，旧 boot／重放／retained 拒绝。

每 5 秒非 retained reported 精确输出 protocol_version、device_id、boot_id、uptime_ms、revision、wifi_state、time_ready、frp_state、last_accepted_event_sequence、last_completed_event_sequence、last_completed_event_sha256、last_event_outcome、last_business_result。无完成事件后三个可空字段为 null，outcome 为 none；完成后 outcome 为 succeeded／business_failed，business_result 为实际 int32。reported 不是持久升级结果。PUBACK 只证明 Broker 接收，须按当前 boot、序号和输入摘要核对设备结果。

保持入站 4,096 B、发布载荷 5,120 B、outbox 16,384 B，以及原网络队列／在途上限。连接／订阅／outbox 过期等故障撤销 ready，再按既有退避恢复；旧 retained online 不能证明当前在线。

## 配置与重启

config.set parameters 为 expected_revision 与完整 schema_version 3 配置（wifi／mqtt／frp／business），business 当前必须为 null。候选取得连接证明后才条件提交、读回；失败恢复已提交配置，不回显凭据。pending／active OTA、配置 trial、重启或存储不确定阻断新的冲突写入。

restart 先回 running，延迟并让已认证回执排出；客户端必须读回同设备的新 boot。FRP 重启最早 100 ms、回执排出最多 2 秒；断线或丢失回执只记未确认。Mac 本机工具须核对实际设备身份、当前连接、操作权限和唯一 USB 租约；远程 Bridge 账号与绑定不再是本机设备操作前置，设备软件不能阻止外部烧录器。

## FRP Base 软件接线边界

唯一 loopback listener 复用既有设备 FRP 工作流。POST `/api/v1/commands/{status,restart,firmware-status,ota-start,ota-result,business-status,business-pause,business-resume}` 使用 application/json、唯一 Content-Length 和 X-ESP-Management-Tag；tag 是独立 FRP 管理 key 对实际 JSON 字节的 HMAC-SHA256。旧 status／restart body 上限 384 B、header 512 B、连接总期限 2 秒保持；新六命令 body 上限 1,024 B，控制期限仍 2 秒。认证前错误只返回空 body，不泄露状态；认证响应 tag 覆盖实际完整响应 JSON。

新六命令 HTTP 映射为 invalid_request→400、failed／expired→409、running→202、succeeded／unknown→200；客户端先验响应 tag、严格字段与设备／请求身份，再读 state。旧 status／restart 映射保持。status 四字段和 8 槽／30 秒首快照缓存保持；新只读命令每次获取当前事实，写请求共用 32 槽守卫。

FRP ota.start 接受五项固件元数据，V4 意图读回后预约唯一 upload。PUT `/api/v1/ota-images/<operation_id>` 必须为 application/octet-stream、精确 Content-Length；HMAC 原文是：

```text
esp-base-ota-upload-v1\n
<operation_id>\n
<device_id>\n
<boot_id>\n
<十进制 image_size_bytes>\n
<小写 sha256>\n
```

其中每行只有一个 LF，首行本身含 LF；正文不由独立第二 MAC 替代完整 signed bin 摘要／签名。上传 header／预读各最多 1,024 B，固定单槽，不把整镜像缓存在 RAM／额外 Flash。控制 owner 验证绑定后移交 fd给唯一 worker，同一 listener 可接另一条查询；占用既有两活跃流预算，不新增第三流资格。

预约 5 秒内建立上传，流单次 I/O 1 秒、无进展 30 秒、总传输 300 秒；每次 Flash I/O 仅持短仲裁，网络等待释放。禁止重复长度、安全头、Transfer-Encoding、Expect、pipeline、第二上传和迟到／重放。Content-Length 界定 EOF；当时已到达的额外字节拒绝，未来字节不作预知，结束关闭该 fd。取消／重配撤销旧 key与连接，借用计数确保最终 close 不作用于复用 fd；验签后选槽前再次检查连接仍有效。

PUT 上传回执只允许 running／202 或 unknown／200，result 均为 null；失败收据尚未持久化时不得回复 failed。客户端对未知、丢失或声称终态的上传响应都只读原 operation_id，不重新提交或上传。准备／选槽不是最终成功，必须在新 boot 经原 operation_id 持久查询并核对独立 firmware.status。当前验收分别覆盖设备 FRP 公网与 Mac 本机有线两条路线；App 内置 FRP／frpc 与远程 Bridge 的删除仍待实施，不再要求远程 Mac Bridge 通过验收。宿主 HTTP／模拟平台不替代公网与实板。
