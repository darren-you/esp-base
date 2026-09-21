# 设备控制协议 v1

本文件冻结 P1 接口；实现验收状态以 README 与测试为准，当前迁入的旧心跳尚未支持以下命令。

## 帧与身份

USB 为 UTF-8 JSON Lines；单帧最大 8192 字节（不含换行），拒绝 NUL、重复 key、未知字段、非法 UTF-8、非对象与非整数数值。只解析带协议字段的行，日志不是 ACK。网络端复用同一请求与结果对象。

只读 `status` 请求包含 `protocol_version:1`、`request_id`、`command:"status"`。响应包含固件持久 `device_id`、随机启动 `boot_id`、`uptime_ms`、配置 `revision`、能力状态与资源事实，不返回秘密。固件初始化失败时不生成替代身份。

写命令必须且仅包含 `protocol_version`、`device_id`、`target_boot_id`、`request_id`、`command`、`expires_at_uptime_ms`、`parameters`。request_id 为规范 UUID v4；target_boot_id 必须精确等于当前启动值，受理期限为当前设备 uptime 后不超过 30000 ms；在出队执行前再次验证。过期拒绝，不跨启动重放。

## 命令与配置

- `config.set`：parameters 为 `expected_revision` 和完整类型化 `config`；只从当前 revision 开始候选事务，校验失败不写入已提交配置。Wi-Fi 候选完成取得 IP 和必要链路 proof 后才提交；超时恢复已提交配置。
- `restart`：parameters 为空对象；发送成功不表示重启成功，必须回读相同 device_id 的新 boot_id。
- `ota.start`：parameters 包含 operation_id、镜像 URL、SHA-256、长度、target 与签名元数据。下载期间不重复执行同一操作；成功以新启动自检确认。
- `business.*`：仅派发业务注册的命令与参数 schema，未知命令拒绝，不提供任意 shell、脚本或 Topic。

配置 schema_version 固定 1，分别包含 wifi、mqtt、frp 和业务 namespace；未配置用 null，不使用空白默认凭据。revision 是设备持久单调整数；状态只返回脱敏字段。USB 与网络进入同一个有界控制队列，配置提交、OTA 和外部 flash 租约互斥。

## 结果与幂等

结果带 protocol_version、device_id、boot_id、request_id、state、error_code、result。state 只允许 accepted/running/succeeded/failed/expired/unknown；error_code 为稳定字符串或 null。所有 key 必须存在。状态以设备最终结果裁决，USB write、HTTP 202、PUBACK 都不是 succeeded。

同 boot 下缓存有界 request_id 与规范内容 SHA-256；同 ID 不同内容返回 request_conflict。缓存满时拒绝新操作，不驱逐尚可被重复投递的有效条目后再次执行。重启后的未终态只能报告 unknown 或基于持久裁决对账，不宣称物理 exactly-once。

## 网络授权与首配

物理 USB 首配绑定真实 device_id；设备管理凭据由维护者的受控材料注入。网络命令使用 HMAC-SHA256，对整个精确 UTF-8 请求字节签名，使用独立管理密钥；先验证身份、签名、boot、deadline 和 request_id 再入队。只在受认证 TLS 路径传递。FRP Token、UUID 或 CORS 都不是管理授权。公开主机调用不依赖工作区 Auth；私有网关另负责维护者/installation 授权与精确目标绑定。

TLS 依赖可信墙钟时间，命令有效期使用设备 uptime；HTTP envelope 的 timestamp 是 Unix 毫秒，两者不得混用。
