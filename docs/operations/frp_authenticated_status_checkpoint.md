# FRP 独立只读状态认证检查点

2026-10-01，设备端软件候选已解除 FRP status 对 USB／MQTT 新鲜 boot、uptime 的前置依赖，并补齐响应认证。当前尚未接入私有网关、受控外侧 HTTPS 或真实 FRPS，不计作 P4-05／P8-08 或实板验收。

## 当前合同

`POST /api/v1/commands/status` 的 JSON 精确为 `protocol_version`、`device_id`、`request_id`、`command:"status"` 四字段，两个 ID 为规范 UUIDv4。旧六字段拒绝；写命令的 boot、30 秒期限和幂等合同不变。使用既有 USB v3 配置中的独立 FRP 管理 key，不改配置 schema 或生成新凭据。

请求 HMAC 在解析前验证。所有有 body 的认证响应由 PSA 计算原始 JSON 字节的 HMAC-SHA256，并通过唯一 `X-ESP-Management-Tag` 返回 64 个小写十六进制字符；设备／当前 boot 身份在七字段 envelope 中，解析通过才回显请求 UUID。认证前空 400／401 不含 tag。客户端先验证原始响应，再验证目标、原请求和业务状态；不能把返回请求 tag、HTTP 200 或 FRP 登录成功当作设备结果。

同 ID 在设备确定的 30 秒 RAM 窗口内返回首次快照，新查询使用新 UUID。八槽容量和总输出 1024 字节保持，响应头预留增加到 256 字节，body 容量为 768 字节；表满、序列化超限、空结果均拒绝。PSA import／compute／32 字节长度／destroy 失败清零输出并关闭连接，不发送未认证结果。查询不写 NVS 或 Flash。

## 软件验证

- C3／ESP32 完整 host ASan/UBSan 回归各 22 个通过摘要，无跳过。新增断言覆盖四字段／旧合同拒绝、设备目标、同 ID 首次快照、新状态、TTL 释放、容量满、768 字节真实 serializer、无写入、签名 200／400／409 与签发失败关闭。
- 显式 `run_frp_status_crypto_tests.sh` 两目标定义均通过：实际 listener、decoder、auth wrapper 与回环 HTTP；OpenSSL 3.6.4 提供测试 PSA HMAC，Python 独立冻结请求／响应向量，检查错 key、tag／body 篡改、认证前空响应及已认证错误响应。已有产品事件 OpenSSL probe 的有效帧／篡改帧继续通过。它们不运行 IDF 密码端口。
- 初轮本地生成 MQTT 缓存落后于精确锁，完整回归因此停止；按既有四仓精确 Git SHA 重新展开生成缓存后通过，没有修改锁或依赖版本。新增签发失败用例首轮无等待轮询遇到 TCP 调度竞态；改为按签发观察在一秒内有界等待，设备两秒总期限保持。host 使用的 cJSON 两个输入与固定 SDK 本轮重新解析的组件逐字节相同。
- 固定 SDK 的两个完整产品签名构建、官方 RSA v2／ECDSA v1 验签和槽容量通过；实际 ELF 中 `ebase_management_sign`、`psa_mac_compute` 与 listener poll 均为已链接代码符号。复用仓外测试键，不使用生产键或改变设备信任根。

| 目标 | 测试签名 app／槽容量 | 完整 app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `2dc80dc7b7a629f7fc3e60a555c0dcf57160b12f94d8541205d34d870b997f40` |
| ESP32 | `0x10fff4/0x120000` | `58cfbc1063ecd8b3e75f4eb0545f5adf2024329364583e8576c521399310a86d` |

SDK 仍为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`；编译器为 ESP GCC 15.2.0，CMake 3.31.10，Ninja 1.13.2.git.kitware.jobserver-pipe-1。MQTT `a46e209cc98c7b910774dbb77d11b34f79492720`、OTA `04acb5e80a744649f8442607fb8d901d30880ca0`、FRP `8f056273b3b93ea3273b4637038ddd0c6aea82a8`、Container `52d94d696d4cb0de3ce6a037c844c16be7edfb54` 与 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 均保持。

构建快照的 302 个普通归档输入在宿主前后保持；其中 215 个非 Markdown 输入与受测候选逐项摘要、执行位一致，组合指纹为 `9cd3665e4a8fede05b6d2f15534e41eba839803c5207abb85de1d989bc552ec2`。测试之后仅调整 Markdown 说明。私有原始日志、SDK 配置、分区、组件摘要及签名构建收据保留在仓外。

## 尚未验证

HMAC 不提供机密性，loopback 仍为 HTTP；外侧受控 HTTPS 入口、隧道 TLS、路由授权、网关 Auth 和绑定必须分别闭合。尚未进行 FRPS 请求到设备、IDF 密码运行、五能力并行、栈／堆测量或两板隔离。仅 C3 当前连接，ESP32 已拔除；本次没有 UART、复位、Flash、eFuse、配置或生产发布操作。
