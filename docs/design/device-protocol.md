# 设备控制协议 v1

本文件为设备协议事实源。当前实现 status、restart、config.set、Wi-Fi 候选验证、UUID 启动身份、有界解析与回执；ota.start 和业务命令仍待实现，收到时返回 unsupported_command。实现与实板证据见开发检查点。

## 帧与身份

USB 为 UTF-8 JSON Lines；单帧最大 8192 字节（不含换行），拒绝 NUL、重复 key、未知字段、非法 UTF-8、非对象与非整数数值。只解析带协议字段的行，日志不是 ACK。网络端复用同一请求与结果对象。

只读 `status` 请求包含 `protocol_version:1`、`request_id`、`command:"status"`。响应包含固件持久 `device_id`、随机启动 `boot_id`、`uptime_ms`、配置 `revision`、能力状态与资源事实，不返回秘密。固件初始化失败时不生成替代身份。

写命令必须且仅包含 `protocol_version`、`device_id`、`target_boot_id`、`request_id`、`command`、`expires_at_uptime_ms`、`parameters`。request_id 为规范 UUID v4；target_boot_id 必须精确等于当前启动值，受理期限为当前设备 uptime 后不超过 30000 ms；在出队执行前再次验证。过期拒绝，不跨启动重放。

## 命令与配置

- `config.set`：parameters 为 `expected_revision` 和完整类型化 `config`；只从当前 revision 开始候选事务，校验失败不写入已提交配置。Wi-Fi 候选完成取得 IP 和必要链路 proof 后才提交；超时恢复已提交配置。
- `restart`：parameters 为空对象；发送成功不表示重启成功，必须回读相同 device_id 的新 boot_id。
- `ota.start`：parameters 包含 operation_id、镜像 URL、SHA-256、长度、target 与签名元数据。下载期间不重复执行同一操作；成功以新启动自检确认。
- `business.*`：仅派发业务注册的命令与参数 schema，未知命令拒绝，不提供任意 shell、脚本或 Topic。

配置 schema_version 固定 1，完整字段为 schema_version、wifi、mqtt、frp、business；P2 的后三项必须为 null，非 null 返回 unsupported_configuration。wifi 为 null 或包含 ssid/password 的精确对象，长度与字符规则见 remote_config README；未配置用 null，不使用空白默认凭据。revision 是设备持久单调整数；状态只返回脱敏字段。USB 与网络进入同一个有界控制队列，配置提交、OTA 和外部 flash 租约互斥。

## 结果与幂等

无法解析或没有合法唯一 request_id 的输入返回 failed/invalid_request，request_id 为 null，不能与任何已提交操作关联。有效请求的结果带 protocol_version、device_id、boot_id、request_id、state、error_code、result。state 只允许 accepted/running/succeeded/failed/expired/unknown；error_code 为稳定字符串或 null。所有 key 必须存在。状态以设备最终结果裁决，USB write、HTTP 202、PUBACK 都不是 succeeded。

同 boot 下缓存有界 request_id 与规范内容 SHA-256；同 ID 不同内容返回 request_conflict。缓存满时拒绝新操作，不驱逐尚可被重复投递的有效条目后再次执行。重启后的未终态只能报告 unknown 或基于持久裁决对账，不宣称物理 exactly-once。

## 网络授权与首配

物理 USB 首配绑定真实 device_id；设备管理凭据由维护者的受控材料注入。网络命令使用 HMAC-SHA256，对整个精确 UTF-8 请求字节签名，使用独立管理密钥；先验证身份、签名、boot、deadline 和 request_id 再入队。只在受认证 TLS 路径传递。FRP Token、UUID 或 CORS 都不是管理授权。公开主机调用不依赖工作区 Auth；私有网关另负责维护者/installation 授权与精确目标绑定。

TLS 依赖可信墙钟时间，命令有效期使用设备 uptime；HTTP envelope 的 timestamp 是 Unix 毫秒，两者不得混用。

## 当前 USB 结果

status 成功的 result 固定含 uptime_ms、revision、free_heap、min_free_heap 和 capabilities；capabilities 固定含 wifi、mqtt、frp、config、ota。尚未接入的能力为 unsupported，配置为 ready（存储结果不确定时为 failed），不能伪造已经内置的 unconfigured 状态。restart 的 running 回执 result 为 null；设备执行重启后通过同 UUID 的新 boot_id 验证完成。

USB 缓冲最多 8192 字节，JSON 嵌套最多 8 层、成员分隔符最多 128 个。拒绝小数/指数数字、NUL（含 Unicode 转义）和重复 key。超限或半帧闲置 2 秒后排空至换行，再处理下一帧；错误输入不回显请求内容或凭据。

USB 使用官方无缓冲 VFS 和硬件 FIFO 背压，不使用可能在 RX ring 满时丢字节的中断缓冲驱动。主机每次请求前后各发送一个换行，前导换行只用于结束先前未完成的帧；不得把写入完成当成设备受理。

## P2 配置执行

`config.set` 的 parameters 必须恰含 expected_revision 和 config；revision 为 0–4294967295 的整数。候选与 expected_revision 一起规范编码，以官方 PSA SHA-256 计算命令指纹，不受 JSON 字段顺序影响。同请求重复返回保存的原始结果，不再次连接或写入；同 ID 不同内容冲突。

USB 配置候选在 RAM 验证最多 20 秒，取得 IP 后核对当前关联；此阶段的必要链路证明是当前 USB 控制通道和候选 Wi-Fi 关联/IP，不宣称互联网、MQTT 或 FRP 已连通。清除 Wi-Fi 则先确认 station 已停止。通过后单 blob 提交 revision 与完整配置，并读回校验；succeeded 的 result 使用与 status 相同的脱敏字段，revision 必须是 expected_revision+1。

候选失败返回 connection_proof_failed，重新选择已提交配置；离线环境不伪造已经恢复连接。候选执行期间其他写命令返回 configuration_busy，status 仍可用。NVS 写后状态不确定返回 unknown/storage_uncertain，不自动重放，不承诺旧配置已恢复；重新读取存储事实后保持写入关闭，重启重新核验。已提交配置的真实断电恢复已验收；候选及 Flash 提交中间态掉电仍待实测。
