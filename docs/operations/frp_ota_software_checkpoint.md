# FRP 固件入站与宿主客户端软件检查点

日期：2026-10-06。对应当前方案 R2／R3 的设备管理监听器、流移交和公网调用端软件子项。维护者本轮决定先完成软件、稍后连接实板；前述单项 host 回归与文末官方 FRPS 宿主组合分别记录；官方 FRPS 本次仅取得回环软件资格，实体 Flash、bootloader、公网脱离 Mac、双板容量或正式发布仍未通过。

## 固定入站机制

`frp_management_listener.c` 保留原 `status`／`restart` 的 384 B JSON、512 B header 和 2 秒期限；新增 `firmware-status`、`ota-start`、`ota-result`、`business-status`、`business-pause`、`business-resume` 固定命令路径，JSON 正文上限 1024 B，继续以独立管理 key 对收到的原字节验 HMAC 后调用 typed handler。请求体重复／未知字段、设备／boot／过期及跨入口互斥继续由同一 command decoder、guard 和控制 owner 裁决。

固件入口为 `PUT /api/v1/ota-images/<operation_id>`，header 最多 1024 B，正文为 `application/octet-stream`，唯一 Content-Length 必须等于已持久登记的完整 signed bin 尺寸。拒绝重复安全／framing header、Transfer-Encoding、Expect、错操作及错尺寸。上传 HMAC 输入精确为：

```text
esp-base-ota-upload-v1\n
<operation_id>\n
<device_id>\n
<boot_id>\n
<image_size_bytes 十进制>\n
<完整 signed bin SHA-256 小写十六进制>\n
```

以上 `\n` 表示单个 LF 字节；输入没有空行或可变空白。三身份均为规范小写 UUID v4。正文实际 SHA-256 和官方签名由 OTA 机制独立验证，metadata HMAC 不替代镜像校验。

唯一 worker 在写前收据已读回后 `upload_arm`，复制本次操作、身份、尺寸、摘要及绑定 key；arm 的 5 秒内认证成功才移交 socket 与最多 1024 B 预读正文。重复／未 arm／已 claim 的连接拒绝，认证失败不取得擦写资格。移交后原 listener 可接只读查询，上传与查询占用原 FRP 活跃流预算，没有新增 listener、旁路 HTTPS 下载或额外不计账的流。

worker 使用 `upload_read` 作为 `eota_stream_t.read`，单次最多 64 B、传入最多 1 秒 socket 等待；它不持有 Flash claim。断流返回失败，单次无数据返回 `-2`；精确 Content-Length 后返回 framing EOF，不要求 HTTP 客户端先 FIN。已收到的额外正文／pipelining 字节拒绝，未来尚未到达的违规字节不能在保留 HTTP 响应的同时预知；连接结束关闭，不把它们作为另一请求或另一镜像消费。OTA 完整 signed 尺寸、摘要和签名仍必须精确相等。

`upload_finish` 在独立的最多 1 秒响应发送预算内使用 arm 绑定的 key 签署准备观察，然后关闭并清除操作。PUT 只返回 `running`／HTTP 202／`result=null`，或 `unknown`／`storage_uncertain`／HTTP 200／`result=null`；两者都不形成持久终态。准备失败仍须由控制 owner 提交并读回原操作结果；只有后继 `ota.result` 能裁决持久成功、失败或未决。客户端收到 PUT 的 `unknown` 继续查询原 ID；即使上传响应声称 `failed`、`expired` 或 `succeeded`，也只按原 ID 对账，不把阶段观察报成最终结果，不重发升级。配置改变 `cancel` 并 shutdown，socket 的短借用计数把最后 close 留给 worker，防止配置任务关闭已复用的 fd；网络等待、HMAC、shutdown 和 close 均在临界区外。取消后的旧事务不会用新配置 key 签响应，无连接、失败或取消的 arm 也必须由 worker finally 调用 finish 释放。

## 实际成本与验证

- 长期 input buffer 从 897 B 增为 **1537 B**，增量 **640 B**；output 仍为 **1024 B**。固定上传 state 包含 **1024 B**预读、**32 B**绑定 key、**32 B**摘要和三 UUID／计数／socket 所有权字段；macOS arm64 host `sizeof` 为 **1256 B**，不能用此数冒充 MCU 的对齐后大小。MCU 实际 BSS、worker 栈和 Flash 最坏争用由 R4／R5 的新制品及实体测量继续登记，成本不回加容量结果。
- 单项 `frp_management_listener_test.c` 在 AppleClang、`-Wall -Wextra -Werror -fsanitize=address,undefined -pthread` 下通过。真实 loopback socket 覆盖旧命令、六新命令 1024 B、错 HMAC、第二上传、原 ID 重放、5 秒迟到、错尺寸／操作、重复 Content-Length、chunked／Expect、预读保留、额外字节、FIN 截断、读取 timeout；上传存活时同 listener 接查询。实际读线程与 configure 并发 shutdown 后读失败，finish 不签旧事务、关闭后可重新接入，未发现 ASan／UBSan 错误。
- `python3 -m unittest discover -s tools -p 'test_frp_ota.py' -v` **16／16** 通过。真实 HTTP 服务验证命令原字节 HMAC、metadata HMAC、9472 B 固件正文完整摘要与固定分块，等待新 boot 的原操作持久成功；新增 PUT `unknown` 后按原 ID 读回持久 `failed`，准备失败但 NVS 不可知时按原 ID 返回 `unknown`，以及上传 `failed`／`expired`／`succeeded` 三种非法终态声明只能由原操作查询确认。每例只提交一次、上传一次。丢失上传响应仍只查原 ID，busy 不上传，查询耗尽不重发，错设备／响应 HMAC 在写前拒绝，错摘要和旧包字段不冒报成功。另有确定性响应头重复／chunked／缺 tag／截断／JSON 重复／NaN、迟到发送、30 秒 idle 和 300 秒 total 上界回归。fixture 的固件是运输测试字节，不具有签名或实板资格。

## 真实宿主调用端

`tools/frp_ota.py` 只依赖 Python 标准库和同仓 `device_control.py` 的共同目标／身份合同。key 从明确环境变量读取，不接受命令行明文 key，不打印其值。`ota-start` 对普通文件完整扫描生成 signed bin SHA 和尺寸，核对文件在扫描／发送前后的 inode、大小与时刻；FRP 提交参数不包含 URL，随后一条 PUT 连接顺序发送同一文件。客户端使用 5 秒连接、最多 1 秒单次 I/O、30 秒发送无进展和 300 秒上传总期限，上传后只读结果查询独立计时。每次升级只提交一次，`unknown` 输出原 operation_id，禁止自动重新提交或新建 ID；上传成功和准备 `running` 不等于最终成功。

```bash
# endpoint 必须指向本轮已核对的设备 FRP 暴露地址；环境变量由授权事实源提供。
python3 tools/frp_ota.py --endpoint http://device-frp.example.invalid:12345 \
  --device-id 22222222-2222-4222-8222-222222222222 --json status
python3 tools/frp_ota.py --endpoint http://device-frp.example.invalid:12345 \
  --device-id 22222222-2222-4222-8222-222222222222 --json ota-start \
  --operation-id 11111111-1111-4111-8111-111111111111 \
  --image-file /absolute/private/signed.bin --target esp32c3/esp_base
python3 tools/frp_ota.py --endpoint http://device-frp.example.invalid:12345 \
  --device-id 22222222-2222-4222-8222-222222222222 --json ota-result \
  --operation-id 11111111-1111-4111-8111-111111111111
```

示例域名、UUID 和文件均为占位，不得据此访问设备。`https` endpoint 使用系统默认 CA 和原主机名验证；设备隧道内部 TLS／HMAC、可信时间、12 sockets 和 FRP 双活跃流合同没有降低。

Base control worker、持久收据的官方 FRPS 宿主组合见下文；SDK 双目标签名构建、Mac Bridge 和 Tool 客户端由当前主线登记各自证据；实板未接入时不勾选 R5／R6，也不启动后置 R7 实际删仓或正式交付。

## 官方 FRPS 与原生 OTA 宿主联调检查点

2026-10-06，本轮 ESP32-C3 与 ESP32 各完成成功、Flash 写入失败、失败收据 NVS 不确定三种场景，合计 6/6 通过。此证据只授予官方 FRPS 回环互操作与宿主事务组合的软件资格，实板、公网、掉电、100 次与 72 小时资格仍须分别验收。

### 实际链路与输入

入口为 [run_frps_ota_interop_test.sh](../../firmware/tests/run_frps_ota_interop_test.sh)，构建器为 [frps_ota_interop.py](../../tools/frps_ota_interop.py)。消费者只连接官方 FRPS 的回环代理端口，执行真实 `FrpClient` 的状态、提交、PUT、上传期间原 ID 查询及终态原 ID 查询。没有直接连接 Base 本地 listener 的消费者旁路。

服务端复用精确 managed efrp 的 `tests/crypto-interop/session.go`：以 `github.com/fatedier/frp/server.NewService` 启动官方服务端，控制与代理均只绑定 `127.0.0.1`。版本从 `frp-service/catalog/runtime-catalog-v2.json` 读取并核对为 `0.71.0`，Go 模块实际选中 `v0.71.0`，其校验为 `h1:hrzMepFp/asl2oAxxLMgffCI/wrzvg6lZyMzxEEnWbQ=`。这是官方服务端源码嵌入一次性 Go 夹具的运行证据，未安装或发布生产 FRPS 二进制。

客户端编译本轮冻结 managed 原件：efrp `989cc876d92b815aeb0b6806fb861f0ee2b39a86`、eota `8ab62f98fba2ea8e76c2822d0e7bf1cb523088a1`。每目标 866 个实际 managed 文件逐个与既有受限软件归档核对，文件集合、组件 hash 及对应目标的 lock 一并核对；ESP32 选择 `dependencies.lock.esp32`。未采用当前 esp-frp 工作树的后续扩展。官方临时 CA、严格 TLS 与现有公共测试 token 合同沿用既有 fixture。

宿主使用官方 MbedTLS 4.1.0 及其原件附带的 TF-PSA-Crypto，源码逐文件摘要在运行前后相等。编译器为 Apple Clang 21.0.0，CMake 4.4.3，Go 1.26.1 darwin/arm64；完整版本、CMake cache、Go 模块及源码摘要在每目标 `inputs/` 中保留。C peer 与真实依赖均启用 AddressSanitizer、UndefinedBehaviorSanitizer。

| 目标 | 源 A 完整 signed 大小/摘要 | 候选 C 完整 signed 大小/摘要 |
| --- | --- | --- |
| ESP32-C3 | 1118208 B / `85e45424b1607beb3258caf63e82406cb563c65b2375658cb02a91b72a8a8ad0` | 1052672 B / `71a4d6ab1d5fd4b6c03828292fce79f3e93660363b4b99ab7ee2df391518b823` |
| ESP32 | 1048564 B / `cd0aa584dccb56f7dabac4ee8708f824cd42b568027980ca7d8c57a630e56b38` | 983028 B / `a0cf903d1ef6462d3875612cc9013af2ac08582037595f0df688975478a0ba4e` |

源 A 只使用此前软件构建的 signed 字节作为宿主 Flash 初始身份，候选 C 使用本轮 final signed 字节；两者精确不同，真实收据的 same-image 拒绝保持。旧 A 不成为本轮原生生命周期测试的已验 A/C pair，也不继承其旧架构或运行资格。每个文件先以有限普通文件读取、核对稳定身份与 chip/project，再保存私有快照；官方 SDK `espsecure verify-signature` 对两目标的 A/C 分别通过，采用各目标既有软件测试公钥。

### 真实执行与明确替身

实际执行 production `esp_base_protocol.c` 的 HMAC 后命令解码、handler、owner admission、异步 `ota_task`、`poll_ota`、业务/control state、storage claim、OTA policy、固件集合观察及 V4 收据组件。生成的 owner shim 只复用现有 owner 测试的外围替身，并移除 eota、收据、PSA、worker 与 listener 的假实现；管理 listener、eota bounded stream、efrp control/work/Yamux/TLS/AEAD 都链接实际源码。

以下为显式宿主替身：DNS 将 fixture 域名映射到回环；POSIX 线程代替 RTOS 调度；两个 0x1e0000 B RAM 缓冲区代替两块 Flash 分区；SDK 分区、selector、状态与 image/signature API；以实际文件写入、`fsync`、读回代替 NVS 驱动；重启及本地健康确认。外围 owner fixture 的设备身份、版本、heap、Wi-Fi、MQTT、可信时间与 FRP owner ready snapshot 同样是测试输入；FRP 传输自身由实际 efrp client/work 运行，未执行 Base `frp_owner.c`、Wi-Fi/MQTT owner 或真实网络恢复合同。宿主 RAM Flash 是测试设备替身，不是固件生产路径的整镜像 RAM 或第三 Flash 副本。SDK image/signature API 的接受是替身，独立官方 `espsecure` 验签不冒充实板 SDK 在线验签。

模拟重启切换到候选 `PENDING_VERIFY`，保留 A 的回滚身份，并重新建立真实 efrp 客户端。经过原有 30000 ms 后，替身调用 SDK mark-valid，再由真实 `esp_base_ota_receipt_record_success` 检查实际完整候选 SHA、selector 和状态，写入并读回 V4 成功收据。没有由测试 handler 直接返回固定成功；此步骤不证明真实 bootloader、`app_main` 健康门或硬件重启行为。

### 结果与边界

| 场景 | ESP32-C3 | ESP32 | 事务与流预算观察 |
| --- | --- | --- | --- |
| 完整成功 | 1052672 B，1028 次 SDK write | 983028 B，960 次 SDK write | 每次 write 最多 1024 B；每次提交只创建 1 个 worker；终态由新 boot 原 ID 查询取得 |
| 写入故障 | 4096 B，4 次成功 write | 4096 B，4 次成功 write | 真实 abort/retire 清理；失败收据提交、读回时 long claim 仍有效，其后才释放；原 ID 返回 failed |
| 失败收据 NVS 不确定 | 4096 B，4 次成功 write | 4096 B，4 次成功 write | long claim 保持占用；原 ID 返回 unknown/storage_uncertain/null，未伪造 failed 或释放成功 |

六场景都在上传期间同时查询原操作，实测 `max_work_active=2`，未放宽双工作流预算；清理后官方代理端口在 5 s 内撤销。上传 prepare 的 running/202 或 unknown/200 均只表达准备阶段，消费者继续原 ID 查询；所有场景重复写次数为零。

此轮联调使用当时的连接 timeout 5 s、单次读 1 s、无进展 30 s、整次 300 s 配置，未逐个耗尽期限；后续发现连接 timeout 会按 DNS 地址重复计时，不能将此配置当作整次连接 5 s 的证明，修复见文末。构建器的单次场景外层期限为 240 s，夹具子流程另有 180 s/150 s 期限；编译阶段 900 s，构建配置与验签阶段 240 s。实际阻塞子孙进程的 1 s 外层超时已确认会终止整个测试进程组并保留日志；相反目标镜像及符号链接输入在服务启动前拒绝。

最终受限证据为本机 `/private/tmp/base_frps_ota_interop_esp32c3_final_20261006` 与 `/private/tmp/base_frps_ota_interop_esp32_final_20261006`，目录与可执行文件权限 0700，日志、输入及元数据文件 0600。包含签名验证、构建、三个场景的完整日志、A/C 快照、实际 V4 字节、HTTP 响应、输入摘要及证据清单。候选正式分发、真实公网入口、实板容量、掉电恢复与长期生命周期均未执行。

| 目标 | receipt.json SHA-256 | evidence_manifest.json SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `923cc44e1f20168e52b03706ed4f5c989a33b219a3d0954de98da0e7140b053d` | `58b90e60fbd9a29e78ec7cc2bb83895e1387a2520b6938f543e2e74a9cb150cc` |
| ESP32 | `a732477b534feba61fe292ea1d4b17b8d9d818b6de16fbfeddc45be649652a59` | `a4652d54df6a033022fbe8c2739f0e7ed8c790b4d37e400a9a88fc2ee48c200f` |

上述两目录全部文件已另存于本仓受限`receipts/private/native_validation_preparation_20261006/frps/esp32c3`与`frps/esp32`，原receipt和manifest字节保持；完整追加归档共1924成员，逐项回读、权限与原软件不变检查通过，manifest SHA为`a1bcbfbde374ae66507acd8bc5bc378d4d0fb911d3528bcaed969d1ad7519713`。同源LAB A/C、有限驱动及真实Paho客户端补验归同一准备归档，边界见[原生软件准备检查点](native_software_checkpoint.md#实板接入前的软件验证准备)。

## 连接期限与真实 Flash 回调补审

后续补审在旧宿主客户端复现多地址连接超期：同一5秒被逐地址重用，两地址红例耗时10秒。当前DNS、逐地址TCP和TLS共用绝对5秒；数字地址直接解析，域名解析进程在超时后kill、wait回收。9项定向回归通过，FRP与有限生命周期驱动联合51项通过，随后真实localhost解析子进程、完整HTTP正文与原ID对账单项通过；不声称一次全52项运行。当前客户端SHA为`fea8b9654541a10393d0af5e1e3a2e3566d01b9ecde501d1bf2acc248a9e42f5`，原ID一次提交和TLS信任合同保持。

此前官方FRPS六场景的Flash owner shim另加pthread mutex，没有执行生产main的短Flash回调，不能据此取得该回调并发资格。直接编译真实main回调后复现共享claim非atomic字段的TSan竞争，现已用局部claim和既有owner CAS闭合交接，两目标完整host ASan／UBSan与独立TSan均通过，包括8192次双线程竞争；500ms BUSY预算保持，没有新增锁或常驻工作区。当前双目标普通配置与LAB A/C已全部新鲜签名构建并归档，不继承旧二进制身份。

当前修复、原失败、新制品与44／1876／502成员归档摘要见[补审检查点](native_software_checkpoint.md#短-flash-交接与宿主连接期限补审)。此轮新增回归只证明所测宿主期限和真实回调交接的软件行为；原6/6回环资格保留原输入，实体Flash时延、MCU调度、实际公网与USB、双板容量、断电及长稳继续等待实板。

## 原 ID 终态与运行固件身份补审

本节对应后继 client SHA `6900a5ac5270d054056ea7206740b386f8a2693b0ec9c405b6aa8cbcafb9906a`。旧 client 的两项新回归产生七个失败断言：相同 boot 的成功、当前运行固件摘要／尺寸／target／槽／字段不符，仍可能被报作升级成功。现在 `ota-start` 必须取得不同于提交时的新 boot 原 ID 持久成功，再独立查询同一新 boot 的 `firmware.status`，四字段与原 ID 收据及本次完整 signed 输入逐项匹配；普通历史 `ota-result` 继续表达所查操作的持久结果，不能被当作当前镜像身份。

终态查询与独立身份核对共享一次绝对截止时间；已到期不打开连接，迟到成功不会再追加五秒核验。单独 `firmware-status` 也严格验证成功 envelope、四字段、非零小写摘要、整数尺寸和对应目标槽上限。成功身份、错类型／几何、迟到、已到期期限及原有上传／认证／未知结果共 28／28 通过；每操作仍只提交一次、上传一次，异常继续原 ID 查询，不自动重发。

固件集合自有物理 read／rollback 查询的短 Flash claim 缺口亦已修复，两目标新定向及完整 host 通过。原失败、当前源码和完整日志保存在 119 成员受限回归档，输入与 manifest 见[本轮补审](native_software_checkpoint.md#固件集合读回与-ota-终态补审)。此前 FRPS 六场景只代表原输入；本轮 scenario 新增新 boot 与独立运行固件四字段断言，并保存 `source_boot` 和实际运行固件响应，新制品联调结果另记，不授实体或公网资格。

### 本轮新制品官方 FRPS 回环结果

修后 71 项生产集合 `bbe264eeb14bc4989169c6a93f6e41ad39c0297b98eacfc71943e19a6ea5c67b` 与上述新 client／scenario 完成 C3／ESP32 各成功、Flash 写入失败、失败收据 NVS 不确定三场景，6／6 通过，两个构建器均真实 exit 0。服务端仍为 catalog 的官方 FRPS 0.71.0，managed／MbedTLS／工具及替身边界沿用上文；这次独立执行，不继承旧六场景结果。peer 的 SDK 物理 read 与 rollback 查询新增短 claim 断言，生产观察源使用本轮修复。

| 目标 | 候选完整 signed／B | 候选完整 signed SHA-256 | 成功 SDK write 次数 |
| --- | ---: | --- | ---: |
| esp32c3 | 1052672 | `6eef9d025f8de61f0646752df0a4ba7595382fa182809dfe466cd5ada8cba897` | 1028 |
| esp32 | 983028 | `4c32cb5e8ce391e07017740582c54ccb7b46679480a739bdf1d473d4927801f3` | 960 |

历史源 A 仍只作为宿主初始 Flash 身份：C3 为 1118208 B／`85e45424b1607beb3258caf63e82406cb563c65b2375658cb02a91b72a8a8ad0`，ESP32 为 1048564 B／`cd0aa584dccb56f7dabac4ee8708f824cd42b568027980ca7d8c57a630e56b38`。两目标 A／候选均另经官方签名核验，旧 A 不构成本轮原生生命周期 pair。

每场景只创建一个 worker，实测工作流最高两个，逐次 SDK write 最大 1024 B；两个失败场景均完成 4096 B／四次成功 write。写失败先提交并读回原 ID 失败收据，再释放长期 owner；NVS 不确定返回 `unknown/storage_uncertain/null` 并保留长期 owner。成功按 `333…` 源 boot 到 `555…` 新 boot 查询原 ID，再独立取得相同新 boot 的实际完整候选摘要／尺寸／target／`ota_1`；随后同 ID 重查一致。过程和最终独立响应保存在各场景 `responses.json`，没有直接伪造成功 handler。

生产 71 项、CLI／fixture 15 项及每目标 866 managed 在运行前后保持。每场景官方代理关闭检查通过，peer 断言 FRP stop／destroy、调用 listener `configure(NULL)`，正常进程退出关闭 listener 描述符；没有直接断言 listener-ready 为 false。初始总结对 listener 清理的措辞过强，保留原总结并在 final 修正，不改测试结果。临时 TLS 目录与本轮所属进程均无遗留，NVS unknown 的长期 claim 按合同保留。summary 记录了读错不存在 scenario JSON 的初始只读失败，原错误工具输出留在当轮聊天，没有独立 raw 失败文件，也没有用错误路径启动场景。

| 目标 | 本轮 receipt SHA-256 | 本轮 evidence manifest SHA-256 |
| --- | --- | --- |
| esp32c3 | `752bbc3c215797fccaf086fe65f0924eed33a20e0adc08a4f510107e20fe62c3` | `a593691e8c5f338973d31b389db28f2f434213729a4e46f2d936a1e714de2256` |
| esp32 | `0ae4d0ef85e0d1066be42b9094ac0fab79e32d00f1f6f77de039b4645fa8fd72` | `84ecae052602fb0dce8cbcb72ccb72505cc922b4b1db3564147daed1deaf0aba` |

final 总结 SHA 为 `844f4a394aa32cb55c54952cd0bc1f205b03f944082fb983329fba6f32382c11`，原证据总清单 SHA 为 `a4c070d37f9ed5531f22061e26a8a7c1ebe7486bc955720f4f0b19cfa1dbb2f9`。软件回环仍不取得真实 FRPS 公网、MCU Flash／bootloader、USB、R5／R6 或正式发布资格。

完整原件已独占保存到 `receipts/private/native_readback_frps_20261006`：1304 个 manifest 成员，manifest SHA `5e96342d8c1a1872465af4e2d83f07533899527333a5bc608e91cf25f08aced0`，archive receipt SHA `4730cf170a7673222cb293a21e0ef47cc09bb51c5e0f55d25854ad4727ab3896`。包括全部 1229 个原证据文件（原总清单的 1228 成员及清单自身）、71 生产源、两目标软件公钥、采集器与归档收据，文件 0600／目录 0700、无链接且逐项读回。没有本轮固件签名私钥或生产凭据；临时 TLS 已清理，公开夹具生成源码与固定软件 token 按原样保存。

独立补审另指出 final 总结中的 `long_ota_owner_success_and_write_failure_released=true` 把成功也写作生产显式 release，措辞过强。实际 production 成功持长期 claim 调用 `esp_restart`，宿主 peer 的模拟新 boot 清零 claim 并重新初始化 owner；只有写失败经过生产 release 并断言 inactive，NVS 不确定按合同断言 retain。原总结与冻结档均未改，最小纠正收据 `receipts/private/native_readback_frps_cleanup_correction_20261006.json` SHA 为 `98ad87a3d3e693387ba07ecb097f90381f31c14658994695b181e343f667992d`，绑定实际源行与摘要；六场景原结果没有被改写或扩大为真实重启资格。
