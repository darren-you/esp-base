# 设备—云协议

设备作为 MQTT 3.1.1 客户端连接 Broker 的 TLS 端口，客户端 ID 是设备 UUID。全部消息是 UTF-8 JSON，使用 QoS 1。鉴权靠 TLS、Broker 账户和 Topic ACL；固件真实性靠镜像签名。[样例](device-protocol-samples.json)是本文的可执行版本，设备和 Server 都用它做解码测试。

## 主题

前缀 `esp-base/<device_id>/`。

| 主题 | 方向 | retained | 内容 |
| --- | --- | --- | --- |
| `status` | 设备 → Server | 是 | 状态文档；遗嘱也发到这里 |
| `cmd/<名称>` | Server → 设备 | 否 | 命令 |
| `rsp` | 设备 → Server | 否 | 命令应答 |
| `ota/desired` | Server → 设备 | 是 | 期望固件；空载荷表示取消 |
| `ota/state` | 设备 → Server | 是 | OTA 状态 |

ACL：设备只能读自己的 `cmd/+` 和 `ota/desired`，只能写自己的 `status`、`rsp` 和 `ota/state`。

## 状态文档

设备在连上 Broker、执行命令之后和每 15 分钟各发布一次。遗嘱是同一主题上的 `{"online":false,…}`，只带 `device_id`、`boot_id` 和 `fw`。Server 重启后读 retained 消息即可得到最近状态。

```jsonc
{"online":true,"device_id":"<uuid>","boot_id":"<每次启动新生成的 uuid>","uptime_s":3600,"reset":"power_on",
 "fw":{"version":"1.0.0+68","sha256":"<完整 signed bin 的 SHA-256>","target":"esp32c3/esp_base","slot":"ota_0"},
 "heap":{"free":178148,"min":160696,"largest":110592},   // 字节；min 是本次启动以来的最小空闲堆
 "stack":{"main":4200,"mqtt_task":2900},                 // 各任务栈的历史最小余量，字节
 "wifi":{"rssi":-52},
 "config":{"revision":3,"error":null},                   // error：最近一次配置试用失败的原因
 "business":{"state":"active","bytes":42},
 "wakeups":57}                                           // 控制任务处理过的事件数
```

## 命令

载荷是带 `request_id`（小写 UUID）的 JSON 对象。设备对每个 `request_id` 只执行一次，重复到达时重发原应答。`request_id` 缺失或格式不对的命令被丢弃。

| 命令 | 参数 | 行为 |
| --- | --- | --- |
| `status` | 无 | 立即重新发布状态文档 |
| `restart` | 无 | 应答发出后重启 |
| `config` | `wifi`:{`ssid`,`password`} 和 `mqtt`:{`uri`,`username`,`password`}，至少一项；`uri` 必须是 `mqtts://` | 应答发出后试用新配置：连上 Broker 才保存并把 `config.revision` 加一；120 秒内连不上则恢复旧配置，`config.error` 为 `trial_failed` |
| `business` | `op`:`add`（另带非空字符串 `data`）、`pause`、`resume` | 原生业务：累计 `data` 的字节数；暂停时 `add` 返回 `paused` |

应答：

```jsonc
{"request_id":"<uuid>","cmd":"business","ok":true,"error":null,"result":{"state":"active","bytes":5}}
```

`error` 取值：`invalid_args`、`unknown_command`、`busy`（已有重启或配置试用在进行）、`paused`、`overflow`。

## OTA

Server 把「这台设备应运行哪个固件」发布为 retained 的 `ota/desired`，设备每次上线都自行收敛，Server 不需要补发。

```jsonc
// ota/desired
{"job":"<uuid>","version":"1.0.0+69","target":"esp32c3/esp_base","size":1052672,
 "sha256":"<完整 signed bin 的 SHA-256>","url":"https://<固件域名>/fw/esp32c3/<sha256>.bin?<期限与签名>"}
// ota/state
{"job":"<uuid 或 null>","state":"downloading","offset":524288,"attempt":1,"error":null,
 "running":{"version":"1.0.0+68","sha256":"<…>","target":"esp32c3/esp_base","slot":"ota_0"}}
```

`state` 取值：`idle`、`downloading`、`verifying`、`rebooting`、`succeeded`、`failed`、`rejected`。下载中每前进 5% 发布一次。

设备规则：

- `sha256` 等于当前运行固件：回报 `succeeded`，不下载。
- 下载用 HTTP Range 从已写偏移继续；偏移每 64 KiB 存一次 NVS，断线或断电后都能续传。
- 网络类错误一律退避后重试（5 秒起，每次翻倍，上限 5 分钟，带抖动；有进展就回到 5 秒），不设总期限。任务期限由 Server 判定，到期后 Server 发布空载荷取消。
- 不可恢复的错误立即 `rejected`，设备不重启：`target_mismatch`、`image_too_large`、`invalid_desired`、`invalid_url`、`size_mismatch`、`sha256_mismatch`、`image_invalid`（镜像结构、芯片型号或签名不通过）、`http_<4xx>`。
- 全部字节写入后，先读回 Flash 核对整镜像 SHA-256，再由 ESP-IDF 验签，都通过才切换启动槽并重启。
- 新固件 300 秒内连上 Broker 才确认有效并回报 `succeeded`；否则 bootloader 回滚，旧固件重新下载。同一任务累计回滚 3 次记为 `failed`，`error` 为 `rollback`。
- 同一 `job` 已是 `failed` 或 `rejected`：只回报原结果。重试由 Server 创建新 `job`。
- 收到空载荷或新 `job` 时，如果还没切换启动槽，就中止当前下载。

成功的唯一判据是新启动上报的 `running.sha256` 等于期望值。
