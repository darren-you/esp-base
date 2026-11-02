# 设备控制协议 v1

生产命令为 status、restart、config.set、firmware.status、ota.start／ota.result、business.status／pause／resume。原生业务直接编译进固件；动态 product.*、包模式和包摘要字段已删除，旧字段或命令按未知输入拒绝，没有兼容分支。软件与实板资格见[执行计划](../operations/ota-allocation-diagnostic-checkpoint.md)及[软件检查点](../operations/native-software-checkpoint.md)。

## 帧、身份与结果

USB 为 UTF-8 JSON Lines，单帧最多 9,216 B（不含换行）；拒绝 NUL、非法 UTF-8、重复／未知字段、非对象、非整数数值以及超深对象。半帧 2 秒后排空至下一换行。所有 UUID 使用规范小写 UUID v4。

USB／MQTT 的只读 status、firmware.status、business.status 精确包含 `protocol_version:1`、`request_id`、`command`。ota.result 另包含 `parameters:{"operation_id":"<原 UUID>"}`，不携带写期限或目标 boot，允许重启后查询原操作。

写命令精确包含 protocol_version、device_id、target_boot_id、request_id、command、expires_at_uptime_ms、parameters 七字段。boot 必须为当前启动，期限为设备 uptime 的安全整数、晚于执行时刻且不超过 30,000 ms。32 槽本 boot 守卫保存首次指纹与 outcome，不驱逐已用记录；同 ID 相同请求只回放，冲突拒绝。表满拒绝新写，旧结果仍可查询。request_id、operation_id 和查询请求 ID 各自保持真实语义。

响应精确为 protocol_version、device_id、boot_id、request_id、state、error_code、result 七字段；状态包含 accepted、running、succeeded、failed、expired、unknown。超时、发送完成和非终态都不等于成功。存储不确定保留原 ID；客户端只能读回，不自动换 ID 或重发写入。

## 固件状态与升级

firmware.status 的 result 精确包含 firmware_sha256、image_size_bytes、target、ota_slot。摘要来自实际运行镜像的完整 signed bin（含签名尾部），在独占 claim 下验签并前后核对 running／boot／槽状态；忙或无法证明时报告 unknown，不使用旧产品状态摘要。C3 target 为 `esp32c3/esp_base`，方案 `esp_secure_boot_v2_rsa3072`；ESP32 为 `esp32/esp_base`，方案 `esp_secure_boot_v1_ecdsa_p256`。

USB ota.start parameters 精确六项：operation_id、image_url、sha256、image_size_bytes、target、signature。image_url 是最多 512 B 的 HTTPS URL；sha256 为非零小写 64 位十六进制；长度必须容纳完整签名镜像并适合目标槽；signature 只含精确 scheme。MQTT ota.start 精确五项，删除 image_url，固件字节通过下述认证块通道提供；带 URL 的 MQTT start 被拒绝。两入口复用唯一升级 owner、V4 意图、验签、备用槽和恢复机制。

受理要求签名策略可用、无本地 pending 或配置试运行／重启，并通过来源 URL／流元数据的静态验证。控制 owner 先取得唯一事务 claim、保留原请求并创建已有 OTA worker；初始 accepted（MQTT）／running（USB）仅表示本 boot 接管，不证明持久登记、完整固件观察或 app 写入。worker 核对已确认运行 A／boot、准确 inactive 槽和前次收据，再持久提交并逐字节读回写前意图；重读并绑定原意图后才使旧 inactive 镜像物理不可启动、开放块请求或分块准备 C。任务创建失败时未尝试登记；登记拒绝不替旧收据写入本次失败，登记／claim 释放不确定时保留门禁和 unknown。精确长度、完整摘要、芯片／项目及官方签名均通过后才选择新 boot。部分失败只有清理 C 且证明 A 仍 VALID／selected 后才能记录 failed，否则 unknown 并保留占用。

ota.result 的 result 精确五项：operation_id、sha256、image_size_bytes、target、target_slot。原收据与实际槽一致时，MQTT worker 完成原意图重读但尚未开放块请求时为 accepted，开放后及其他活动 worker／pending C 为 running；C 为 VALID、完整身份吻合且成功收据提交读回才 succeeded；A 为 VALID／selected 且原失败收据已完成才 failed。只看 INVALID／ABORTED 或看见 VALID 而缺成功收据都不能推断终态；未登记／被后续操作替换的 ID 为 unknown。普通未签名构建拒绝固件写入与持久结果资格。活动 worker 重读并绑定原收据后，仅将精确目标槽发布给控制 owner；进度查询复用当前请求的原身份，不再与长 Flash 验证争抢读取。该内存观察只输出非终态，worker 结束后的查询继续核对持久收据与实际槽。

worker 登记或重读期间，精确目标槽尚未发布时，ota.result 返回 unknown／operation_busy、result 为 null；不争抢持久观察或把尚未证明的槽填入结果，也不允许另一操作取代当前 owner。此状态不表示可以重发 start。登记完成前断电，不承诺该新 ID 已持久；恢复仍只依据实际原收据。

新 boot 不依赖 Broker 在线确认：本地初始化与控制进展通过完整 30 秒窗口，确认 VALID 并复核后提交成功收据。控制进展跨窗或失活重新累计窗口；确认／读回不确定保持原事实。普通 app OTA 不改变分区表。

V4 为 182 B 固件独立记录。旧 V3、旧长度、损坏、未知或读取失败一律存储不确定；不能自动清空或改成新操作。首次有线迁入审计旧终态、受限归档原收据与物理证据后才在离线候选中退役旧运行键，保留 UUID／配置／revision／凭据。布局／镜像身份替换后旧原 ID 在设备返回 unknown，其历史通过迁入收据核对；新 OTA 的原 ID 持久查询合同保持。未决或损坏迁入阻断，不通过清 NVS 解决。

## 本机启动观察

本机串口每 5 秒的 `ESP_BASE_REPORTED` 日志在同一行绑定 device_id、boot_id 和 `boot_ready`。`boot_ready=true` 来自控制状态中的启动验证门已解除；main 只在本地初始化、必要的 pending 验证／持久收据恢复和启动 storage claim 释放成功后解除该门。它只授予消费者发起独立固件查询的准备依据，不代替完整 signed 镜像身份或升级成功收据。此字段不改变 MQTT reported 的精确 JSON 合同。单次 `ESP_BASE_READY` 日志可能在主机重开串口前丢失，不能要求主机一定捕获它。

## 原生业务

business.status result 精确三项：byte_count、state（idle／active／paused）、window_deadline_uptime_ms。business.pause／resume 使用七字段写身份与空 parameters；复用请求守卫。暂停保留计数并清定时窗口，恢复只允许 paused→idle。新启动计数、状态和最近事件重置，不持久化暂停。

MQTT 原始业务字节沿用消息计数样例：首字节 0x01 加至少一个数据字节，按后续字节数累计（含零字节）；0x02 暂停；0x03 恢复；0x04 返回状态数值 0／1／2；0x05 返回计数。除 0x01 外长度必须为 1。idle 的首个计数事件开启 100 ms active 窗口，后续事件不延长，到期变 idle且计数保持。非法输入 -1，暂停拒绝 -2，int32 计数溢出 -3，时间溢出 -4；不新增硬件动作。

业务同步借用当前认证事件内存，无新 FIFO、heap 副本或任务；有限处理完成后记录实际输入 SHA-256 和业务返回。业务负值属于已处理失败，序号推进，避免同一业务拒绝被重复当作未交付。认证／序号／资源准入失败不推进序号。

UART 为每个 boot 最多记录一次业务帧拒绝、前向序号缺口和业务处理器拒绝；认证层另外保留第一次 PSA 失败的阶段与错误码，业务摘要失败保留第一次错误码与输出长度。日志不包含密钥、tag 或业务内容，不改变认证、序号或重传规则。同 boot 重连与重配不清除事件诊断标记，避免持续缺序导致日志刷屏；这些日志用于故障归因，不作为业务成功收据。

## MQTT 网络命令合同（固件软件接线候选）

MQTT 3.1.1／严格 TLS 复用既有 CA、主机名、当前 boot 时间门和设备凭据。ClientID 为设备 UUID，精确 command／event 两订阅 SUBACK 均批准后才 ready。命令须精确 Topic、QoS 1、非 retained；载荷为 64 个小写 HMAC-SHA256 十六进制字符、LF 和原始 JSON，认证后才解码。网络 config.set 返回 physical_usb_required。

event 采用 `esp-base-business-event-v1\n` 域隔离。认证原文依次为该 ASCII 域、36 B device UUID、36 B 当前 boot UUID、8 B 大端正 event_sequence 和原生业务字节，外层仍是 64 B tag 与 LF。完整入站帧最多 4,096 B，所以业务字节最多 3,924 B；没有包摘要。序号必须为本 boot 下一值，重连与同 boot 重配不清高水位，旧 boot／重放／retained 拒绝。

每 5 秒非 retained reported 精确输出 protocol_version、device_id、boot_id、uptime_ms、revision、wifi_state、time_ready、last_accepted_event_sequence、last_completed_event_sequence、last_completed_event_sha256、last_event_outcome、last_business_result。无完成事件后三个可空字段为 null，outcome 为 none；完成后 outcome 为 succeeded／business_failed，business_result 为实际 int32。reported 不是持久升级结果。PUBACK 只证明 Broker 接收，须按当前 boot、序号和输入摘要核对设备结果。

保持入站 4,096 B、发布载荷 5,120 B、outbox 16,384 B，以及原网络队列／在途上限。连接／订阅／outbox 过期等故障撤销 ready，再按既有退避恢复；旧 retained online 不能证明当前在线。

## MQTT OTA 二进制块合同

`mqtt_ota.c` 与 Server 的 `mqttcontrol/ota_frame.go` 使用以下 v1 数据帧。设备源码已把 MQTT owner、单块状态机和 OTA worker 接通；正式 Server 调度、组件精确版本更新、完整固件构建及实板验收尚未完成，不能据此宣称线上 MQTT OTA 已可用。交付资格统一由 #35 验收。

全部帧使用 QoS 1、非 retained，完整帧仍小于既有 4096 B 入站上限。设备仅在现有已认证、已持久登记的 OTA operation 内发送 `esp-base/<device UUID>/ota/request`；Server 在 `esp-base/<device UUID>/ota/data` 回复。两个方向均复用 MQTT management key；topic ACL 和 HMAC 分别核验。

| 顺序 | 字段 | 编码 |
| --- | --- | --- |
| 1 | tag | 32 B 二进制 HMAC-SHA256，覆盖后续所有字节 |
| 2 | 域 | ASCII `esp-base-ota-v1` 加一个 LF |
| 3 | kind | 1 B：1=request、2=data、3=end、4=cancel |
| 4 | device、boot、operation | 各 36 B 规范小写 UUID v4，无分隔符或 NUL |
| 5 | offset | 大端 uint32，下一请求的完整 signed 镜像偏移 |
| 6 | written_offset | 大端 uint32，实际顺序 Flash 写入并释放短 claim 后的累计字节，不能大于 offset |
| 7 | expires_at_uptime_ms | 大端 uint64，设备本 boot 的非零截止时刻 |
| 8 | length | 大端 uint16，后续 data 的精确字节数 |
| 9 | data | 仅 kind=2 时非空，最多 2048 B；其他 kind 必须为零长度 |

Server 精确回显当前 request 的 offset、written_offset 和 expiry，再附上对应数据。当前软件使用 2048 B 单块窗口，完整最大帧为 2223 B，保持在原 4096 B 入站上限内；OTA 核心顺序写缓冲仍为 1024 B。块大小与写入粒度分别判断，传输不设总期限；吞吐及双板资源资格须对应完整候选实测。旧 1024 B 固件与当前 Server 不混用，先经已授权的本机 URL 引导装入匹配候选，再验收 MQTT；不自动协商或回退。

块通道不调用普通 32 槽写请求守卫，也不写 NVS。接收方只保有一个 2048 B 块和当前／上一帧身份；在 MQTT 借用内存失效前完成复制。认证相同的重复帧幂等，同一请求的冲突数据使该活动传输失败；错 offset、错期限、超长、迟到和过期帧拒绝。合法重复或重连重发不能推进偏移或刷新期限。只有实际写入确认可推进 written_offset，并刷新 30 秒无进展期限。

数据读尽不等于落盘；下一块请求等待上一块的真实写入确认。最后一个传输块内每完成 1024 B Flash 写入仍须等待真实确认；END 请求的 written_offset 为完整长度向下取整到 1024 B，不能在最后一次完整写入前冻结较旧确认。仅剩余不足 1024 B 的写缓冲按 OTA 核心合同等待显式 END，因此 offset 可等于完整长度而 written_offset 仍小于该长度。END 认证后才允许 EOF／不足 1024 B 尾部写入。任何块接收、应用写入确认或 PUBACK 都不表示完整摘要／签名通过、选槽或新 boot 持久成功；仍按原 ID 收据与独立 firmware.status 对账。

单块状态机不执行网络、Flash 或 NVS；实际 owner 必须在短临界区串行调用并在区内取单调时钟。worker 等待数据时释放临界区和 Flash claim；结束并确认 worker 已停止后才能清理／复用状态。首版只有同一活动 operation 内的易失续传，断电不保留偏移、不自动重发 start，也不将 unknown 改为失败或成功。

MQTT owner 增加第三个精确订阅 `ota/data`，继续由 SDK 完成全部 SUBACK 后开放管理。它在唯一已登记 operation 上绑定处理器；重连保留绑定，配置撤销解除绑定。数据块经设备／boot／operation、topic、方向和 HMAC 校验后直达固定 mailbox，不进入普通写请求守卫。出站最多保留一个未 PUBACK 的块请求；相同请求收到 PUBACK 后至少间隔 1 秒才能重发，新写偏移可立即请求。普通断线不清除尚在 SDK outbox 的消息 ID，停止／销毁 transport 后才清除，避免重连或 operation 更替造成请求积压。

worker 先重读 V4 意图并退役备用槽，再开放块请求；`written` 回调只在实际写入和短 Flash claim 释放成功后推进确认。准备完成后以同一 mailbox 临界区检查 END、全部写入、取消／冲突和期限，并关闭网络取消入口，再调用已有 `eota_select`。该边界之前的有效取消禁止选槽；边界之后的迟到取消无效，客户端继续查询原 operation 的持久结果。入站 read 在暂无数据时于锁外至多让出一个既有 tick 后返回可重试 timeout，由 OTA 核心再次读取并检查原期限；不在单次回调内耗尽读取预算。MQTT 使用 1 秒单次读取和 30 秒实际写入无进展期限，不设总传输时限。只有严格增加的 Flash 写入偏移刷新无进展期限；重复块、PUBACK、心跳、重连和已过期输入均不能刷新。OTA 核心启用 `no_total_timeout`，与来源 mailbox 使用相同的实际写入进展语义；启动授权保持原期限。软件、生产候选和实板资格分别验收。

独立固定向量位于 `firmware/tests/testdata/mqtt-ota-v1.txt`，测试 key 为公开的字节 0..31。C 与 Go 各持有相同内容，SHA-256 为 `8b60d5dc13154989f32c3130cafdc49ae02fadc8ef7cd17131927519d8d6cdb6`；它们是测试数据，不是设备身份或生产凭据。

## 配置与重启

config.set parameters 为 expected_revision 与完整 schema_version 4 配置（wifi／mqtt／business），business 当前必须为 null。候选取得连接证明后才条件提交、读回；失败恢复已提交配置，不回显凭据。pending／active OTA、配置 trial、重启或存储不确定阻断新的冲突写入。

restart 先回 running，延迟并让已认证回执排出；客户端必须读回同设备的新 boot。断线或丢失回执只记未确认。Mac 本机工具须核对实际设备身份、当前连接、操作权限和唯一 USB 租约；远程 Bridge 账号与绑定不再是本机设备操作前置，设备软件不能阻止外部烧录器。

## 旧配置与退役边界

分区表与闲置 scratch 原字节保持；旧 v3 配置只经严格校验移除退役字段，保留 revision、Wi-Fi/MQTT 和旧签名固件可读格式，提交后独立 SHA-256 读回。具体合同见 [remote_config](../../firmware/components/remote_config/README.md)。历史运行证据由 Git 与 Issue #35 保留。


## Flash 调用与 MQTT 共同观察

生产诊断用 `ESP_BASE_FLASH_CONTEXT` 将本 boot 内不复用的整数 context 绑定原 `operation_id`。请求 boot 同时绑定原 `request_id`；重启恢复只从 V4 原收据取得 operation，输出 `request_id=none`，不改 182 B 格式或伪造前 boot 请求。映射在 Flash/MQTT 锁外输出。独立调用序号在同一 boot 内不复用；嵌套、计数耗尽或倒退时钟使观察永久失效，不能改变业务结果。

通用 OTA 的可选只读回调包围实际 SDK 的退役擦除、begin、app write、完整镜像验签、end、选槽、确认与失效调用；Base 另分别标识登记、失败和成功收据的 `nvs_set_blob` 与 `nvs_commit`。`set_blob` 本身可能写 Flash，commit 包围区间不能冒充全部 NVS 写入。回调只维护固定整数，不打印、不分配、不调用 MQTT，不修改调度或期限策略。固定静态存储、回调栈、短临界区和输出调度成本仍实际存在并计入。调用包围区间含 begin 返回与实际调用前等调度间隙；不等同于 Flash 电脉冲、纯验签耗时或整个阶段持续满载。

MQTT 只在实际未完成 DATA 回调中，于共同 owner／outbox 扫描前后读取活动 context。两个完整整数身份相同且有效时，才将这次实际 tuple 交给 Base；历史全局峰不参与。Base 使用 16 个固定阶段槽；来源／恢复的 image verify 与 prepare 候选验签、select 候选复验分别记录，登记早期峰不能授予后继候选验签资格。selector API 可能选择目标或恢复旧槽，单凭该样本不区分两者，须另核最终槽读回。Base 每阶段保留一份完整共同样本，先比较 owner 字节，再比较该样本的 outbox；相等时取更新样本，不能拼接两个独立最大值。回调晚到但仍属最后一次调用时可保留，已切到另一调用或 context 时保守丢弃，不误绑定。没有观测到样本表示缺证，不推断阶段没执行。

`ESP_BASE_FLASH_JOINT` 给出阶段、调用身份及包围边界、实际样本时段、runtime 实例、完整与未完成 owner、outbox、metadata 和固定观察成本。原五秒控制报告只安排 dirty 行；既有 control pass 每轮至多输出一行，操作结束或重启前才同步 flush。未绑定时不重复旧样本。全部打印发生在物理 Flash claim／MQTT API 锁之外；观察成本计入真实候选的 RAM、栈与运行时，不从门限加回。

严格入口为 `python3 tools/analyze_flash_joint.py --boot-id <UUID> --operation-id <UUID> --request-id <UUID> --uart-log <原件>`；恢复 boot 省略 request 参数。它拒绝跨身份、重用 ID、截断原件、错误字节几何、样本越过调用边界、历史回退与失效后恢复，不自动授完整 R5。`analyze_flash_phases.py` 继续只分析旧聚合／外层阶段，新的共同记录由专用解析器核对。堆 region 和栈仍用正式完整容量帧及连续历史下界覆盖样本时刻；分次读取不宣称原子“同一时刻全部资源快照”。业务 HMAC／序号／确认、完整候选与 outbox 最大合法窗口仍需独立对账。

实际可达性决定联合范围：旧 boot 的网络 OTA 可与写入、验签、选槽及登记／失败收据共同观察；同一物理 Flash claim 保护的不同 SDK 调用本来互斥，不能要求它们同时执行。新 boot 在本地 pending 自检、确认和成功收据期间，`poll_network_owners` 跳过整个 MQTT owner；成功落账后才清除 pending，因此这些阶段与 MQTT runtime 结构上不能同存。保留这个启动顺序，不为采峰提前开放网络；新 boot 的正确确认／持久成功继续独立验收，不能用旧 boot 的四帧代替。
