# 设备控制协议 v1

本文件为设备协议事实源。当前实现 status、restart、config.set、受控签名构建的 ota.start/ota.result、Wi-Fi 候选验证、UUID 启动身份、有界解析与回执；业务命令仍待实现。普通未签名构建收到合法 OTA 命令时返回 `ota_signing_unavailable`。实现与实板证据见开发检查点。

## 帧与身份

USB 为 UTF-8 JSON Lines；单帧最大 9216 字节（不含换行），拒绝 NUL、重复 key、未知字段、非法 UTF-8、非对象与非整数数值。只解析带协议字段的行，日志不是 ACK。网络端复用同一请求与结果对象。

只读 `status` 请求包含 `protocol_version:1`、`request_id`、`command:"status"`。响应包含固件持久 `device_id`、随机启动 `boot_id`、`uptime_ms`、配置 `revision`、能力状态与资源事实，不返回秘密。固件初始化失败时不生成替代身份。

只读 `ota.result` 请求精确包含 `protocol_version:1`、`request_id`、`command:"ota.result"`、`parameters:{"operation_id":"<UUID v4>"}`；它不携带写入期限或目标 boot，允许在新启动后按原 operation ID 读取结果。响应 `request_id` 对应本次查询，`result` 含 `operation_id`、完整 signed bin `sha256`、`image_size_bytes`、固定 `target` 和 `target_slot`。设备身份仍以响应的 `device_id` 由调用方核对。

写命令必须且仅包含 `protocol_version`、`device_id`、`target_boot_id`、`request_id`、`command`、`expires_at_uptime_ms`、`parameters`。request_id 为规范 UUID v4；target_boot_id 必须精确等于当前启动值，受理期限为当前设备 uptime 后不超过 30000 ms；在出队执行前再次验证。过期拒绝，不跨启动重放。

## 命令与配置

- `config.set`：parameters 为 `expected_revision` 和完整类型化 `config`；只从当前 revision 开始候选事务，校验失败不写入已提交配置。Wi-Fi 候选完成取得 IP 和必要链路 proof 后才提交；超时恢复已提交配置。
- `restart`：parameters 为空对象；发送成功不表示重启成功，必须回读相同 device_id 的新 boot_id。
- `ota.start`：parameters 精确包含 `operation_id`（UUID v4）、`image_url`（最多 512 字节 HTTPS URL）、`sha256`（完整 signed bin 的小写 64 字符十六进制）、`image_size_bytes`（完整镜像字节数）、`target`（固定 `esp32c3/esp_base`）、`signature`（精确对象 `{"scheme":"esp_secure_boot_v2_rsa3072"}`）。签名构建要求当前运行槽 VALID、boot 与 running 一致、另一 OTA 槽可写、Wi-Fi IP 和本次启动时间同步。目标 otadata 只允许历史 VALID/INVALID/ABORTED/UNDEFINED 或尚无记录；NEW/PENDING/读取异常拒绝写入。启动下载任务前先把设备 ID、operation ID、摘要、长度与旧/目标槽写入 `base_store/base_ota/operation` 并逐字节读回；写入不确定时拒绝下载。下载期间不重复执行同一操作；切槽后先报告 running 并重启，成功须待新槽本地自检完成、otadata 为 VALID 且运行镜像完整摘要匹配。
- `ota.result`：签名构建查询最近一次登记的 operation ID。worker 活跃或目标槽 pending 时为 `running`；目标槽运行且 VALID、完整镜像摘要匹配时为 `succeeded`；已持久记录的下载失败或目标槽 ABORTED/INVALID 且旧槽有效时为 `failed`；收据缺失/损坏、槽关系不明或仅见旧槽而无失败证据时为 `unknown`。普通未签名构建拒绝查询。
- `business.*`：仅派发业务注册的命令与参数 schema，未知命令拒绝，不提供任意 shell、脚本或 Topic。

配置 `schema_version` 固定 3，完整字段为 `schema_version`、`wifi`、`mqtt`、`frp`、`business`。Wi-Fi 为 null 或精确 `{ssid,password}`；MQTT 为 null 或精确 `{hostname,port,username,password,ca_pem,management_key_hex}`；FRP 为 null 或精确 `{server_hostname,server_port,token,ca_pem,proxy_name,remote_port,local_port,management_key_hex}`；business 必须为 null。FRP Token 为 1–256 字节非空可打印 ASCII，CA PEM 最多 2048 字节，proxy_name 为 1–128 字节受限 ASCII，三个端口均为 1–65535；本地目标固定为 `127.0.0.1`，独立管理 key 与 MQTT key 不互用。主机为 1–253 字节 ASCII DNS 名（单 label 最多 63 字节），端口为 1–65535 整数；用户名 1–128 字节、密码 1–256 字节，均为无控制字符的 UTF-8；CA PEM 1–4096 字节，含证书 BEGIN/END 标记，只允许可打印 ASCII 与 tab/CR/LF；管理密钥为非全零的 64 个小写十六进制字符，解码后独立保存 32 字节。Wi-Fi 长度规则见 remote_config README；未配置用 null，不使用空白默认凭据。revision 是设备持久单调整数；状态仅返回现有脱敏字段，MQTT/FRP 能力按实际 owner 状态报告。USB 控制任务使配置候选/提交与 OTA 下载互斥；外部串口 Flash 租约只能由工具侧管理，设备不能阻挡外部刷写。

## 结果与幂等

无法解析或没有合法唯一 request_id 的输入返回 failed/invalid_request，request_id 为 null，不能与任何已提交操作关联。有效请求的结果带 protocol_version、device_id、boot_id、request_id、state、error_code、result。state 只允许 accepted/running/succeeded/failed/expired/unknown；error_code 为稳定字符串或 null。所有 key 必须存在。状态以设备最终结果裁决，USB write、HTTP 202、PUBACK 都不是 succeeded。

同 boot 下缓存有界 request_id 与规范内容 SHA-256；同 ID 不同内容返回 request_conflict。缓存满时拒绝新操作，不驱逐尚可被重复投递的有效条目后再次执行。重启后的未终态只能报告 unknown 或基于持久裁决对账，不宣称物理 exactly-once。

OTA 收据只保存最近一次 operation。相同 operation ID 永不重新下载：摘要/长度相同返回 `ota_operation_exists`，不同返回 `ota_operation_conflict`。前次结果未能裁决时，新 ID 返回 `ota_previous_unresolved`，不能覆盖唯一持久证据；可能需要外部恢复后才能继续 OTA。一个新操作仅在前次有成功或失败证据时覆盖收据。目标状态不安全、selector 不一致、当前槽非 VALID 或目标状态读回异常分别拒绝并返回 `ota_target_not_safe`、`ota_selector_mismatch`、`ota_source_not_valid` 或 `ota_target_state_unknown`。`ota.result` 不重放写动作；查询旧 ID 在收据被新操作替换后返回 `unknown/ota_operation_not_found`。回滚若进入尚未实现 `ota.result` 的旧镜像，该镜像无法读取新收据，工具必须报告 unknown，不能推断失败或成功。

## 网络授权与首配

物理 USB 首配绑定真实 device_id；设备管理凭据由维护者的受控材料注入。网络命令使用 HMAC-SHA256，对整个精确 UTF-8 请求字节签名，使用独立管理密钥；先验证身份、签名、boot、deadline 和 request_id 再入队。只在受认证 TLS 路径传递。FRP Token、UUID 或 CORS 都不是管理授权。公开主机调用不依赖工作区 Auth；私有网关另负责维护者/installation 授权与精确目标绑定。

TLS 依赖可信墙钟时间，命令有效期使用设备 uptime；HTTP envelope 的 timestamp 是 Unix 毫秒，两者不得混用。

## MQTT 网络命令合同（固件软件接线候选）

正式设备的 ClientID 仍是持久 UUID。四个 Topic 精确为 `esp-base/<device_id>/command`、`result`、`reported`、`status`；静态段为小写 kebab-case，设备 UUID 原样填入。设备只读 `command`，只写其余三个 Topic；控制端只对已绑定设备写 `command`、读 `result`/`reported`/`status`。Broker 必须为每台设备分配独立 principal 和精确 ACL，不借用实验应用的 `esp-base-lab` Topic 或已有共享 principal。

首版 MQTT 3.1.1 只走严格 TLS。设备取得 Wi-Fi IP 和本次启动可信时间后才启动客户端；只有本轮 `command` QoS 1 订阅的 SUBACK 已批准，才可报告 MQTT `ready`。`status` 的上线和 LWT 离线消息采用 QoS 1 retained，载荷分别为 `{"protocol_version":1,"device_id":"<UUID>","boot_id":"<UUID>","state":"online"}` 和同结构的 `state:"offline"`；retained `online` 只是最近提示，Broker 重启或设备主动停止/重配会话后可能残留，Tool/网关不得由单条 retained 消息判在线。MQTT ready 时，`reported` 每 5 秒以 QoS 1 非 retained 发布 `{"protocol_version":1,"device_id":"<UUID>","boot_id":"<UUID>","uptime_ms":<整数>,"revision":<整数>,"wifi_state":"<状态>","time_ready":<布尔>,"frp_state":"<状态>"}`，不包含连接密码、管理密钥等敏感配置。Tool/网关按自身收到该次消息的时间、当前会话及 boot_id 判断新鲜度；设备 uptime 不是 Unix 时间，历史 reported 也不能单独证明当前在线。

`command` 载荷是连续 `64` 个小写十六进制字符、一个 LF、原始 UTF-8 v1 JSON 请求字节，总长度最多 `4096` 字节。前缀解码为 32 字节 HMAC-SHA256 tag，使用独立的设备管理密钥，只覆盖 LF 后的原始请求字节；不重排 JSON、归一化空白或先解析再签名。仅精确 `command` Topic、QoS 1、非 retained、格式和 HMAC 均有效的消息进入既有 JSON decoder、boot/deadline、request_id/指纹裁决。无认证消息直接丢弃，不回显 request_id 或产生 ACK。已认证但语法错误的请求由设备协议结果裁决。DUP 重投不重复执行，复用本次 boot 的原结果；跨 boot 未决结果仍是 unknown，不能从 PUBACK 推断成功。

`result` 发布与 USB 相同的设备结果对象，QoS 1 且非 retained；只有设备执行状态可以是 `succeeded`。同启动重复请求经共同 owner 的幂等裁决后回送原结果；异步 OTA 结果回送原请求通道。发布入队、Broker PUBACK 和 `status=online` 都不是操作终态。`config.set` 的 MQTT 凭据、CA 与独立管理密钥只能由受控物理 USB 注入；已认证的远端 `config.set` 经身份、期限与去重裁决后返回 `failed/physical_usb_required`，不写入配置。普通固件的软件 owner 已接入客户端、订阅和结果通道；设备级 Broker ACL、Tool 的网络控制端与实板 v1/v2→v3 迁移尚未生效，此代码构建与 host 测试不构成网络端到端验收。

QoS 1 outbox 报告消息过期时，设备立即撤销 MQTT `ready`、停止会话并在退避后重新取得 `command` SUBACK。该过期事件只证明传输回执丢失，不改写已执行命令的结果；同一 boot 的控制端可用相同 request_id 重投，写命令由原去重表回送已保存结果，只读命令重新查询设备事实。重启后仍须按新 boot 与持久事实裁决，不能把旧 request_id 当作跨启动的执行证明。

## 当前 USB 结果

status 成功的 result 固定含 uptime_ms、revision、free_heap、min_free_heap、ota_received_bytes、ota_total_bytes 和 capabilities；capabilities 固定含 wifi、mqtt、frp、config、ota。未签名构建的 OTA 为 unsupported；签名构建在空闲时为 ready、下载时为 running。配置在存储或启动槽不确定时为 failed。restart 的 running 回执 result 为 null；设备执行重启后通过同 UUID 的新 boot_id 验证完成。

`ota.result` 的 `state` 和 `error_code` 是查询时由持久收据与当前槽事实裁决的结果；一次 `ota.start` 的 running 回执以及 USB 写入成功均不是最终成功。NVS 登记写入或失败终态持久化不确定时返回 `unknown/storage_uncertain` 并关闭本次启动的配置写入，不能自动重试升级。目标槽摘要读取失败时返回 `unknown/ota_result_uncertain`。

USB 缓冲最多 9216 字节，JSON 嵌套最多 8 层、成员分隔符最多 128 个。拒绝小数/指数数字、NUL（含 Unicode 转义）和重复 key。超限或半帧闲置 2 秒后排空至换行，再处理下一帧；错误输入不回显请求内容或凭据。

USB 使用官方无缓冲 VFS 和硬件 FIFO 背压，不使用可能在 RX ring 满时丢字节的中断缓冲驱动。主机每次请求前后各发送一个换行，前导换行只用于结束先前未完成的帧；不得把写入完成当成设备受理。

## P2 配置执行

`config.set` 的 parameters 必须恰含 expected_revision 和 config；revision 为 0–4294967295 的整数。候选与 expected_revision 一起规范编码，以官方 PSA SHA-256 计算命令指纹，不受 JSON 字段顺序影响。同请求重复返回保存的原始结果，不再次连接或写入；同 ID 不同内容冲突。

USB 配置候选在 RAM 验证最多 20 秒，取得 IP 后核对当前关联；此阶段的必要链路证明是当前 USB 控制通道和候选 Wi-Fi 关联/IP，不宣称互联网、MQTT 或 FRP 已连通。清除 Wi-Fi 则先确认 station 已停止。通过后单 blob 提交 revision 与完整配置，并读回校验；succeeded 的 result 使用与 status 相同的脱敏字段，revision 必须是 expected_revision+1。

候选失败返回 connection_proof_failed，重新选择已提交配置；离线环境不伪造已经恢复连接。候选执行期间其他写命令返回 configuration_busy，status 仍可用。NVS 写后状态不确定返回 unknown/storage_uncertain，不自动重放，不承诺旧配置已恢复；重新读取存储事实后保持写入关闭，重启重新核验。已提交配置的真实断电恢复已验收；候选及 Flash 提交中间态掉电仍待实测。

OTA pending 新槽完成本地确认前，`config.set` 在身份、期限与去重裁决后返回 `failed/ota_verification_pending`，不执行候选连接或配置提交；下载期间返回 `ota_in_progress`。`status` 保持可读。确认成功后新 request_id 可执行配置写入，原 request_id 重放仍返回原失败结果。签名构建的 OTA 下载与配置候选互斥；外部 flash 租约仍须工具侧实现。

## FRP Base 软件接线边界

普通 Base 精确锁定公开 `esp-frp@9158b7f2e2c555a14636aed26b5189902152d19e`。FRP owner 在单一 USB 控制任务中持有一个客户端句柄；配置变更、网络或时间门失效时用非阻塞 destroy 持续收敛，未完成时保留句柄。`ready` 只来自库完成代理注册与首轮认证 Pong 的状态快照，status 与 reported 不含 Token、CA、管理 key。受控 listener 与 owner 共用该控制任务，只在配置的 `127.0.0.1:local_port` 绑定；绑定失败保持 `endpoint_unavailable`，不会向 FRPS 建连。配置 revision 变化时先关闭旧 listener 与半帧，旧 FRP worker 销毁后才装配新独立 key。FRP Token、TLS 和 UUID 不能代替管理端点授权。

当前设备端点仅有只读软件候选：HTTP/1.1 `POST /api/v1/commands/status`，请求必须含一个非空 `Host`、精确 `Content-Type: application/json`、十进制 `Content-Length` 和 `X-ESP-Management-Tag`。Tag 为 `frp.management_key` 对**原始 JSON body 字节**计算的 HMAC-SHA256，以 64 个小写十六进制字符发送；先完整读取并验证 HMAC，之后才解析或回显 request ID。header 最多 512 字节，body 为 1–384 字节，不接受重复安全/长度头、`Transfer-Encoding`、`Expect`、HTTP 管线化或无效帧；单连接从 accept 起的读取与写回总期限为 2 秒，随后关闭。未认证请求返回空 body 的 HTTP 401，错误 HTTP 帧返回空 body 的 400；两者均不泄露设备状态。listener 同时只处理一个连接。

body 精确包含 `protocol_version:1`、`device_id`、`target_boot_id`、`request_id`、`command:"status"`、`expires_at_uptime_ms` 六项；三个 ID 都是规范 UUIDv4。设备在同一控制任务核对持久设备 ID、当前 boot ID 与单调 uptime，期限必须晚于本轮 uptime 且不超过 30 秒；8 槽有界表保存未过期 request ID、期限与首次脱敏快照。同 ID 同一规范请求在期限内返回首次结果，同 ID 不同目标/期限返回 `request_conflict`，过期请求按 `expired` 拒绝后释放槽位；表满时拒绝新请求。已认证但非法的请求返回现有结果 envelope、HTTP 400；错设备/错 boot/过期/冲突返回相同 envelope、HTTP 409；成功返回 HTTP 200、`succeeded` 与 USB/MQTT 共用序列化的脱敏 status `result`。该端点不接受 restart、OTA、配置写入或任意命令；HTTP 状态码与 body 的 `state` 分别表示本次请求的传输/业务裁决。

FRP status 要求调用方先从**本轮** USB 或 MQTT 新鲜状态取得 boot ID 与 uptime，不能从 FRP 单一路径首次自举；这是当前受限切片的明确边界。设备本地 loopback 为明文 HTTP，外侧调用方仍须经受控 HTTPS 入口和 FRPS 的严格 TLS/路由授权验证；该软件编译没有验证外侧路径、真实 FRPS、同板 MQTT/OTA 并行、资源门槛或硬件运行，因此 P4-05 尚未验收。
