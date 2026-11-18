# 设备—云协议

设备作为 MQTT 3.1.1 客户端连接 Broker 的 TLS 端口，客户端 ID 是设备 UUID。这条连接是 esp-base 的系统 MQTT，地址和账户只在 USB 刷写时由出厂资料写入，不能远程修改：改错或改到别的 Broker，Server 就再也管不到这台设备。业务需要 MQTT 时另开一条连接，连接信息经 `config` 命令的 `business_mqtt` 下发。全部消息是 UTF-8 JSON，使用 QoS 1。鉴权靠 TLS、Broker 账户和 Topic ACL；固件真实性靠镜像签名。[样例](device-protocol-samples.json)是本文的可执行版本，设备和 Server 都用它做解码测试。

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

状态文档就是设备的 Profile：设备能报告的全部信息。设备在连上 Broker、执行命令之后、取得 IPv6 地址时和每 15 分钟各发布一次。遗嘱是同一主题上的 `{"online":false,…}`，只带 `device_id`、`boot_id`、`name`、`group` 和 `fw`；名称或 Group 改了之后设备会重建连接，让 Broker 记住新的遗嘱。Server 重启后读 retained 消息即可得到最近状态。

```jsonc
{"online":true,"device_id":"<uuid>","boot_id":"<每次启动新生成的 uuid>","uptime_s":3600,"reset":"power_on",
 "name":"实验板 C3",                                       // 没设置时为「型号-MAC 后 3 字节」，例如 ESP32-C3-4A1B2C
 "group":"<uuid>",                                         // 所属 Group；没有归属时为 null
 "fw":{"version":"1.0.0+68","sha256":"<完整 signed bin 的 SHA-256>","target":"esp32c3/esp_base","slot":"ota_0"},
 "chip":{"model":"esp32c3","revision":"v0.4","cores":1,"cpu_mhz":160,"features":["wifi","ble"],"flash":4194304,"psram":0},
 "idf":"v6.1",                                             // 构建固件的 ESP-IDF 版本
 "heap":{"free":178148,"min":160696,"largest":110592},     // 字节；min 是本次启动以来的最小空闲堆
 "stack":{"main":4200,"mqtt_task":2900,"mqtt_task_2":3100}, // 各任务栈的历史最小余量，字节；同名任务加序号
 "wifi":{"rssi":-52,"ssid":"<SSID>","bssid":"aa:bb:cc:dd:ee:ff","channel":6},
 "net":{"mac":"aa:bb:cc:dd:ee:01","hostname":"espressif",
        "ipv4":{"address":"192.0.2.20","netmask":"255.255.255.0","gateway":"192.0.2.1"},  // 没有地址时为 null
        "ipv6":["fe80::…","2001:db8::…"]},                 // 链路本地与 SLAAC 地址，最多 3 个
 "mqtt":{"uri":"mqtts://<Broker 域名>:8883"},              // 系统 MQTT；只报地址，不报账户和密码
 "business_mqtt":{"uri":"mqtts://…"},                      // 业务 MQTT；没有配置时为 null，同样不报账户和密码
 "config":{"revision":3,"error":null},                     // error：最近一次配置失败的原因，trial_failed 或 not_saved
 "wakeups":57}                                             // 控制任务处理过的事件数
```

`chip.features` 取值：`wifi`、`ble`、`bt`、`ieee802154`、`emb_flash`、`emb_psram`。`name`、`group`、`chip`、`idf`、`net`、`mqtt`、`business_mqtt` 和 `wifi` 的 `ssid`／`bssid`／`channel` 是带 Profile 的固件新增的，Server 对旧固件的状态文档继续接受（样例 `online_legacy`、`offline_will_legacy`）。Server 严格解析状态文档，所以加字段时先发布认识新字段的 Server，再升级固件；删字段顺序相反。

设备端由 `base_profile` 组件持有 Profile：芯片、MAC 和 IDF 版本启动时采集一次，名称、Group、MQTT 地址与配置版本由控制任务写入，堆、栈、Wi-Fi 和 IPv4／IPv6 由一个低优先级任务按请求异步采样，采完再由控制任务发布。状态文档和遗嘱只从 Profile 快照编码（`base_status_encode`、`base_will_encode`），不在别处另行采集。

## 命令

载荷是带 `request_id`（小写 UUID）的 JSON 对象。设备对每个 `request_id` 只执行一次，重复到达时重发原应答。`request_id` 缺失或格式不对的命令被丢弃。

| 命令 | 参数 | 行为 |
| --- | --- | --- |
| `status` | 无 | 立即重新发布状态文档 |
| `restart` | 无 | 应答发出后重启 |
| `config` | `wifi`:{`ssid`,`password`}、`business_mqtt`:{`uri`,`username`,`password`}、`name` 和 `group`，至少一项；`uri` 必须是 `mqtts://`；`name` 是 1–32 字节 UTF-8，不含控制字符，首尾不是空格；`group` 是小写 UUID，`null` 表示移出 Group；其他字段（包括改系统 MQTT 的 `mqtt`）一律 `invalid_args`，不部分执行 | `business_mqtt`：业务层另开连接用的 Broker，esp-base 不连接它。`name`、`group`：设备资料，Group 本身只在 Server／网页里管理，设备只存 ID。这三项收到即保存并把 `config.revision` 加一，保存失败时 `config.error` 为 `not_saved`；改了 `name` 或 `group` 时应答发出后重建系统 MQTT 连接，更新遗嘱。`wifi`：应答发出后试用新网络，连上 Broker 才保存并把 `config.revision` 加一；120 秒内连不上则恢复旧配置，`config.error` 为 `trial_failed` |

应答：

```jsonc
{"request_id":"<uuid>","cmd":"restart","ok":true,"error":null,"result":null}
```

`error` 取值：`invalid_args`、`unknown_command`、`busy`（已有重启或配置试用在进行）。`result` 留给有返回值的命令，现有命令都没有，固定为 `null`。

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
- 下载用 HTTP Range 从续传点继续。下载途中续传点每 64 KiB 存一次 NVS，断电后从这里继续；断线或停滞后从已写入的最后一个 4 KiB 扇区起点继续。
- 连续 20 秒收不到数据视为停滞（`error` 为 `stalled`），设备主动断开，不等这条连接自己恢复。
- 网络类错误和停滞一律退避后重试（5 秒起，每次翻倍，上限 5 分钟，带抖动；续传点前进了就回到 5 秒），不设总期限。任务期限由 Server 判定，到期后 Server 发布空载荷取消。
- 不可恢复的错误立即 `rejected`，设备不重启：`target_mismatch`、`image_too_large`、`invalid_desired`、`invalid_url`、`size_mismatch`、`sha256_mismatch`、`image_invalid`（镜像结构、芯片型号或签名不通过）、`http_<4xx>`。
- 全部字节写入后，先读回 Flash 核对整镜像 SHA-256，再由 ESP-IDF 验签，都通过才切换启动槽并重启。
- 新固件 300 秒内连上 Broker 才确认有效并回报 `succeeded`；否则 bootloader 回滚，旧固件重新下载。同一任务累计回滚 3 次记为 `failed`，`error` 为 `rollback`。
- 同一 `job` 已是 `failed` 或 `rejected`：只回报原结果。重试由 Server 创建新 `job`。
- 收到空载荷或新 `job` 时，如果还没切换启动槽，就中止当前下载。

成功的唯一判据是新启动上报的 `running.sha256` 等于期望值。
