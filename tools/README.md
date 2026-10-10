# ESP Base 宿主工具

运行在 macOS／Linux 宿主，不进入 MCU 固件。设备操作必须使用本轮精确 UUID、boot、端点与唯一租约；以下占位不指向真实设备。软件与实板进度见[唯一计划](../docs/operations/ota-allocation-diagnostic-checkpoint.md)。

## 设备控制与升级

`device_control.py` 复用独占串口、严格 JSON、精确设备／boot 和原 ID 查询。命令为 status、firmware.status、business.status／pause／resume、restart、config.set、ota.start／ota.result。旧 product.* 与联合包选项在打开串口前拒绝，没有 wrapper。

```bash
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE status
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE firmware.status
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE business.status
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE --device-id <本轮UUID> business.pause
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE --device-id <本轮UUID> --operation-id <固定原UUID> --image-file /absolute/private/signed.bin --image-url https://example.invalid/signed.bin --target esp32c3/esp_base ota.start
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE --operation-id <固定原UUID> ota.result
```

有线 OTA 是下发 URL 后由设备 HTTPS 拉取完整 signed bin；软件扫描本地文件得到摘要／长度，提交前核对同设备、当前 boot 与 OTA 可用状态。当前 USB CLI 提交后返回 running／unknown；持久结果须另按原 operation_id 查询，当前镜像身份须独立 `firmware.status` 核对，不能把这些分开的调用当成已自动完成新 boot 成功对账。写只提交一次，超时／unknown不重发、不另建操作。ESP Tool Mac App 的当前职责是本机有线管理，使用这些设备命令；其内置 FRP／frpc 与远程 Bridge 正按最新计划硬切删除，新本机调用链另行验收，不再作为正式使用路线。官方有线刷写／恢复由对应宿主能力执行，不能与 app OTA 混同。

`frp_ota.py` 经明确设备 FRP 地址执行认证控制、固件 PUT 和原 ID 查询，固件正文不走设备直连 HTTPS。持久成功还须来自不同于提交时的新 boot，随后独立 `firmware.status` 必须在同一新 boot 精确匹配完整摘要、尺寸、target 与真实 OTA 槽；原 ID 查询和身份核对共用原终态绝对期限。迟到、错身份或未知结果保留原 ID，不重发。HTTP／HMAC／期限和命令示例见[FRP 软件检查点](../docs/operations/frp_ota_software_checkpoint.md)。独立 FRP 不要求 Mac／USB 在线；宿主 loopback 测试不证明已经经过正式 FRPS或实板。

宿主控制命令的 DNS、TCP／TLS 建连、发送和完整认证响应共用原始 5 秒绝对期限；调用方给出的更早期限继续生效，不在 TLS 后重新计时或压缩为 2 秒。设备 loopback 控制连接的 2 秒期限及正文上界保持原合同；该设备端期限不等于公网往返预算。`ota.start` 响应丢失或超过宿主原期限时保留原 operation_id 为 unknown，不发送 PUT、不重发 start，后续只查原 ID。

`--endpoint` 可以包含已登记的规范设备路径前缀，例如 `https://example.invalid/devices/esp-base-esp32c3`；客户端在同一前缀下追加 `/api/v1/commands/*` 和 `/api/v1/ota-images/<operation_id>`，上传后的原 ID 查询与独立镜像身份核对仍使用这个前缀。前缀各段只接受小写字母、数字和连字符，不接受尾斜杠、空段、点段、编码路径、query、fragment 或空白字符，也不会发现其他设备路线。根路径仍用于明确的独立入口及隔离宿主互操作；HTTPS 保持系统 CA 和原主机名验证。`native_lifecycle_run.py` 复用同一客户端，因此百次／72 小时驱动消费相同显式入口。

## 原生 MQTT 业务事件

`business_event.py` 从私有普通事件文件生成域隔离 HMAC帧；key 文件须0600、非符号链接，不回显key。`business_event_publish.py` 通过严格 TLS／QoS1／非 retained 仅发布一次，先核对本 boot reported 的下一连续序号，随后按 boot、序号、原始事件摘要和实际 business result 对账。输入只绑定原生业务身份。

完整 event frame 最多4096 B，业务原始字节最多3924 B；输入语义与13字段reported见[设备协议](../docs/design/device-protocol.md)。Broker PUBACK不是设备执行成功；丢失结果不自动重发业务或换序号。

## 构建与回归

```bash
python3 -m unittest discover -s tools -p 'test_business_event*.py' -v
python3 -m unittest discover -s tools -p 'test_device_control.py' -v
python3 -m unittest discover -s tools -p 'test_frp_ota.py' -v
python3 -m unittest discover -s tools -p 'test_*partition_table.py' -v
```

分区测试使用 IDF_PATH 指向锁定SDK，调用官方生成器核对两目标4MiB几何、签名对齐、双app、scratch／NVS和ESP32只读AT区。实板未连接时不写串口或Flash。

## SDK 源码准备

本仓 `sdk-lock.json` 锁定公开 ESP-IDF fork 的 OTA 擦除失败和 HTTP 初始化低内存清理修正，以及公开 esp-lwip 的零窗口修正。首次准备独立 SDK 时，将 `ESP_BASE_IDF` 指向仓外的新路径：

```bash
ESP_BASE_IDF=/private/path/esp-base-idf
git clone --recurse-submodules --branch codex/fix-http-init-transport-oom \
  https://github.com/darren-you/esp-idf.git "$ESP_BASE_IDF"
git -C "$ESP_BASE_IDF" checkout --detach 578cf89c343e388db43ba1f4ddcd602fedcb763c
git -C "$ESP_BASE_IDF" submodule update --init --recursive
git -C "$ESP_BASE_IDF/components/lwip/lwip" fetch \
  https://github.com/darren-you/esp-lwip.git 2758df4cd3666b3b2a5b53830148379326425c0d
git -C "$ESP_BASE_IDF/components/lwip/lwip" checkout --detach FETCH_HEAD
bash "$ESP_BASE_IDF/install.sh" esp32c3 esp32
source "$ESP_BASE_IDF/export.sh"
python3 tools/prepare_sdk.py --path "$IDF_PATH"
python3 tools/check_sdk.py --path "$IDF_PATH"
```

`sdk-lock.json` 的 schema 2 保持 IDF `578cf89`、lwIP `2758df4` 和官方 TLSF `2867f68` 的精确基线，显式声明两份容量统计补丁、补丁摘要及每个受影响文件的原始／最终摘要。SDK 根唯一 `esp-sdk-derivation.json` 必须逐字等于该冻结 lock；装配在全部原件、Git 状态和两仓 `git apply --check` 通过后才应用，再排他创建只读 stamp。它不覆盖旧 SDK，不删除失败后的 partial，也不清除未知差异。

构建核对受管修改、SDK 索引与工作树、TLSF 和全部其他子模块及最终解析的 lwIP 组件路径。原始 SDK、坏 stamp、部分补丁、未知或暂存修改、子模块漂移、补丁／源码摘要不符均拒绝；不能声称派生 SDK 未修改。Git remote 使用 HTTPS 或 SSH 不改变提交身份。C3 使用 `firmware/dependencies.lock`，ESP32 使用 `firmware/dependencies.lock.esp32`；二者分别固定 target，不能共用生成的 sdkconfig/build 目录。以上入口不访问串口或设备。三个组件的精确派生消费锁是正式双目标构建前置；单独装配 SDK 或 host 通过不代表完整固件已构建。

## 正式容量统计

正式原生源码始终编译 `esp_base_capacity.c`，要求非 ROM TLSF、单核非 SMP、官方 trace 实例编号，禁用 poisoning、task-owner 字节和 LAB 观察副本。统计不是 malloc hook 或定时 heap 遍历：TLSF 每个真实单 pool region 在原分配器锁内维护最低连续合法普通申请和最低总空闲，覆盖真实分配、calloc 下层、aligned／定址、原地及移动 realloc、free 和初始化。移动 realloc 的新旧块共存必须记录；失败／零申请不制造空值。最高有效 TLSF bin 经官方 `tlsf_fit_size` 得到与官方 largest 相同的普通申请边界，更新为恒定成本，不扫描链表。

诊断输出读取真实注册 region 的范围、三优先级 caps 和地址 alias，一个物理堆只计一次。延迟启动堆和动态出生 region 的缺席前缀按零处理；它们仍报告自身注册以来的最低值，但不能抬高从堆初始化起的保证。匹配 caps 的 `sum(region min_free)` 与 `max(region min_largest)` 分别是总空闲和最大单次申请的严格保守下界，均不称全局精确历史峰值。五个实际域包含 `INTERNAL|8BIT`、`INTERNAL|32BIT`、`INTERNAL|DMA`、`DEFAULT` 和 ESP32 配置工作对象使用的 `INTERNAL|IRAM_8BIT`。

`alignment_bytes=4` 仅表示受管无 owner／poison、非 EXEC 的普通合法申请。真实后续请求必须另外绑定大小和 caps；更高 alignment、硬件对齐、EXEC alias 前缀或其他开销须按锁定分配路径单独证明，不能以 24 KiB 数字替代。历史保证不承诺查询后存在并发时下一次申请仍成功。

SDK 在每个已停止任务的 `prvDeleteTCB` 中、最终上下文保存之后且清理／释放栈之前采官方 HWM，静态、自删和他删都覆盖。所有创建／最终采集实例计数、最差真实实例及全局最低值保留；名称只作展示，不去重。存活与待清理实例由官方完整列表核对，必须满足 `created = finalized + allocated`。正常清理间隙和列表容量不足不授该帧截止资格，保留原始帧，等待后续计数闭合与累计最终 HWM 补齐前缀；末尾不完整帧或不完整 `before_reset` 不延长可证截止。编号／计数溢出和无效计数仍拒绝。当前双目标 `StackType_t` 为 1 B，输出仍显式按其大小转换。

每五秒既有 control pass 及四个既有主动重启点前输出完整帧，不增业务命令、任务、队列或动态内存。各 heap getter 异时读取，因此共同可证区间截至 `BEGIN uptime_ms`，不延伸到 END、打印后的尾段、硬复位、panic 或丢失的 UART 帧；新 boot 不补旧 boot 资格。正式解析使用 `analyze_capacity.py`，旧 LAB 解析入口仍不授正式资格。运行收据还须绑定实际 boot 和当前完整 signed candidate，数值门通过不自动授 R5／R6。

统计成本全部留在新候选：TLSF 每 region 12 B、新 heap 出生字段的真实 padding、trace 每 TCB 8 B、SDK 全局 36 B、32 个存活 TaskStatus／名称缓冲、诊断代码和栈，以及任务终态 HWM 扫描的时间。必须按目标实际 ELF／map 和操作测量报告；任何费用都不加回空闲或栈余量。

```bash
IDF_PATH=/absolute/managed-sdk python3 tools/test_managed_sdk.py
python3 tools/test_sdk_capacity.py --idf-path /absolute/managed-sdk
```

前者先实际核对 SDK，再以隔离夹具验证拒绝边界；后者直接编译受管 SDK 的 TLSF 和实际任务最终采集片段，包含 100000 次碎片化操作、逐稳定状态 walk oracle 与移动 realloc 共存反例。host 及 sanitizer 结果不替代 MCU、正式签名或双板容量验收。

正式诊断使用既有 stdout／VFS，沿用已配置的 USB Serial/JTAG／UART no-driver 路径；不直接使用缺少源码超时保证且不支持 long long 的 ROM formatter。C3 VFS 在无连接时返回 EIO，FIFO 无 host 读取时按最后成功输出起 50 ms 后丢字节；完整帧仍须由原始日志确认。UART 按实际字符数和 115200／8N1 计线路费用，不等待 host ACK；SDK FIFO轮询、stdio锁、USB慢读和 MCU 调度成本仍须实测，不以线路预算冒充最坏时延。丢帧或丢尾不得补成覆盖。

```bash
python3 tools/analyze_capacity.py --target esp32 \
  --boot-id <本轮bootUUID> --sdk-lock-sha256 <冻结sdk-lock文件SHA256> \
  --uart-log /absolute/private/raw-uart.log --json
python3 -m unittest discover -s tools/tests -p 'test_analyze_capacity.py' -v
```

解析只接受完整原始 UART 行，绑定 boot／SDK lock、帧序号、region 和任务实例，复算保守下界并明确截至 BEGIN；不会接受文本前缀、跨文件残帧、未知字段、坏计数或最低值回退。任务生命周期只以最近两个完整快照为锚点，正常不完整快照不据缺失实例推断退休，也不丢弃原始日志。38 项定向软件回归与真实 producer 输出组合不授予真实下一申请、R5／R6 或 MCU时延资格。

需要对照具体后续申请时，在同一调用增加 `--request-evidence /absolute/private/request-evidence.json`。不带该参数的正常行为、JSON 与退出码不变。证据采用以下 strict JSON；示例中的路径、boot 和摘要须替换为本轮原件，大小是实际单次申请字节数，caps 是完整 SDK 位掩码的整数，不能只写一个较弱域：

```json
{
  "schema_version": 1,
  "target": "esp32",
  "boot_id": "11111111-2222-4333-8444-555555555555",
  "sdk_lock_sha256": "<本轮冻结 sdk-lock.json 的 64 位小写 SHA256>",
  "allocator_recipe": "managed_tlsf_plain_32bit_v1",
  "identity": {
    "signed_firmware": {"path": "/absolute/private/complete-signed.bin", "sha256": "<完整文件 SHA256>"},
    "config": {"path": "/absolute/private/consumed-config.json", "sha256": "<配置原件 SHA256>"},
    "sources": [{"path": "/absolute/frozen-source/consumer.c", "sha256": "<源码原件 SHA256>"}],
    "firmware_status_uart": {"path": "/absolute/private/firmware-status.uart", "sha256": "<原始 UART 文件 SHA256>"}
  },
  "requests": [
    {"id": "internal-buffer", "size_bytes": 26000, "caps": 2052, "alignment_bytes": 16}
  ]
}
```

`firmware_status_uart` 可省略，其余字段必需。所有引用须是稳定普通文件的绝对路径及精确摘要，拒绝符号链接、FIFO、改写、重复 JSON 字段、未知字段、重复请求 id、空请求／源码集合、布尔数值和非 power-of-two 对齐。文件内容不输出；配置与源码只计算摘要。firmware.status 原件仅接受固件输出的完整 JSON 行，核对同 boot、target、device、slot、完整 signed SHA256 和文件大小；不以文件名或截断摘要代替。多个 firmware.status 记录须身份一致。

请求证据的 target／boot／SDK 必须与调用及原始容量帧一致。原始容量帧没有 target、完整 signed、配置或源码摘要，故 target 在没有同 boot status 时仍由调用方提供；配置文件摘要只证明宿主原件，源码摘要只证明已引用文件，不能据此声称设备消费或 signed 构建输入已经绑定。输出 `identity.fully_bound=false` 及明确 `unbound_reasons`，可选 status 的核对结果只称 `observed`。完整 signed 文件的摘要核对也不代替独立签名与发布验收。

parser 固定并核对当前 sdk-lock、正式 producer 和两份受管 patch 的原件摘要，不接收调用方公式或任意 `maximum_verified`／`coverage_complete` 声明。当前 recipe 仅支持无 owner／poison、non-EXEC、无硬件 caps 改写的 `32BIT`／`8BIT`／`INTERNAL`／`DEFAULT`／`IRAM_8BIT` 完整掩码组合。双目标指针和 size_t 为 4 B，TLSF minimum block 为 12 B、block header 为 16 B：普通请求先取 `adjust=max(12, align_up(size, 4))`；alignment 低于 4 B 时按 4 B；高于 4 B 时使用真实 `tlsf_memalign_offs` 的 `search_size=align_up(adjust+alignment+16, alignment)`。DMA、EXEC alias、cache、descriptor、SIMD、其他未建模 caps 路径及目标整数溢出报告 `unknown`，不能使用 plain 域偷授资格。calloc 应提供乘积已核对的总字节数，移动 realloc 应提供新块实际申请；并存旧块费用仍由独立生命周期证据覆盖。

每项从最近完整任务帧的原始 REGION 重新匹配全部 caps，三优先级求并集，一个物理 region 只计一次，晚出生 region 不贡献从启动开始的下界。只有其 `max(region min_largest)` 覆盖 search-size 才输出 `status=fit`；下界不足输出 `unknown`，不推断实际申请必然失败。结果包含原始 size／caps／alignment、search-size、总空闲与连续申请下界、margin、最弱 margin／请求 id 和可证 BEGIN 截止。不完整尾帧不能延长请求证明。`fit` 仅表示该单个已提供请求在锁定 recipe 与已读历史下界下的充分条件，不承诺并发后的未来成功。

结果保存在 `provided_request_fit`，全局 `next_maximum_legal_request_verified`、`r5_qualified`、`r6_qualified` 始终为 false；`next_request_unclosed_reasons` 明示全合法消费者集合、动态真实输入、并发／realloc 生命周期和固件配置源码消费绑定仍未封闭。CLI 退出码仍只反映输入有效性与三个数值门；调用方须读取各请求 status，不得将退出码 0、`all_provided_requests_fit` 或一份 sidecar 当作全局资格。

## 一次性有线迁入与历史输入

`preflight_v3_migration.py` 仍只做旧C3 v1/v2配置的只读预检，不是当前原生布局迁入。`archive_esp32_at.py` 核对两份完整备份，按 NVS 前三页和 `at_customize` 前两页生成 20 KiB 原字节归档；两分区余尾必须全 FF，并逐字节重建完整原分区，不解码或打印凭据。ESP32 候选将其放入只读 `at_old_raw@0x3e5000/0x5000`。旧 `esp32_product` 的四页输入仅在一次性离线准备时补入第三 NVS 的 FF 页，运行时只消费当前五页布局。新布局的一次性离线准备工具只在审计原终态、归档回读与完整输入验证后生成候选，不触达设备；未决／损坏保持阻断，不能清空NVS。最终精确命令与软件边界见[原生软件检查点](../docs/operations/native_software_checkpoint.md)。

旧 AT 空 Wi-Fi 判断先核对 blob 类型与旧 SDK 的 36／65 B 字段长度，再只核对声明 payload 是否全 FF；NVS 槽尾 padding 不属于凭据，但仍随五页归档原样保留。有效 payload 的非 FF 字节、类型／长度不符或 CRC 损坏均阻断。

原生候选／公钥／分区输入、旧C3双备份和现役AT双备份的读取先以非阻塞方式打开，再核对普通文件及原权限／尺寸规则；无写入方的FIFO会明确拒绝，不会停在打开阶段。完整受影响回归35项及原失败见[输入拒绝补审](../docs/operations/native_software_checkpoint.md#一次性迁入输入拒绝补审)；本工具仍只准备离线输入，不能代替本轮实体身份、正式信任、恢复基线和唯一租约。

`prepare_native_layout.py --source-layout c3_mqtt_factory` 用于独立 MQTT 实验固件首次迁入原生 Base：仅接受 C3 的 `nvs@0x9000/0x6000`、`phy_init@0xf000/0x1000`、`factory@0x10000/0x100000` 三分区，核对完整未签名 `esp_mqtt_broker_client` 镜像、checksum／摘要和擦除态槽尾。NVS 与 `0x3e0000` 后的旧 Base 持久范围必须全为空；有数据则阻断，不能据此清空旧身份或配置。来源没有 Base UUID，只记录本轮独立读取的规范 `--source-efuse-mac`，实际物理绑定须另核对双份恢复基线和写前 ROM 身份。原 factory app 占用的新 otadata／PHY／coredump 区在候选中重新装配，原始完整字节先归档并读回。新 UUID 由首次启动的设备生成，Wi-Fi 经当前 USB 配置链设置；该离线工具不连接或写入设备，也不授予正式交付资格。

旧动态包生命周期、专属NVS容量探针与旧QEMU组合入口已删除。当前离线准备只消费明确SDK、真实旧布局终态与签名固件，工具与本轮设备写入资格分别核对。

## 资源与独立 MQTT 实验

`capacity_observation.py` 从实验UART读取顺序周期堆／完整任务快照／退出记录，保持16384／24576／1024 B门；必须匹配真实观察器的完整启动声明、明确 target、唯一启动及非递减 uptime。缺失、错目标、重复启动、uptime 回退、损坏或不完整数据不通过，跨文件也不合并不同启动轮。原日志只读并保存摘要。它不是瞬时峰值或完整native生命周期资格，观察器成本不加回。

旧 `prepare_capacity_observer.py` 及 `LAB_ONLY` 5 秒观察仅解释已冻结历史副本；当前正式源码拒绝 `ESP_BASE_CAPACITY_OBSERVER_LAB`，不要从活动 canonical 再生成该副本。`capacity_observation.py` 可离线解析旧日志，始终不授完整峰值或生产资格。旧 pre-deletion／存活采样、64 条退出缓存、日志丢失及异常重启的边界仍保留，不能把旧数值移给正式统计新候选。原件不改写，也不跨日志或 boot 拼接。

`mqtt_lab_check.py`／`mqtt_resource_report.py` 继续用于独立MQTT实验的严格TLS、往返、计数／栈和回收。实验输入必须位于仓外，不混入普通Base，独立实验不替代本轮FRP／MQTT／原生业务／OTA同存验收。

## 官方 FRPS 宿主互操作

[frps_ota_scenario.py](frps_ota_scenario.py) 是 macOS／Linux 主机消费者，与 `frp_ota.py` 同目录导入；[frps-ota-interop](../firmware/tests/frps-ota-interop/) 是 C／Go 主机联调夹具的职责目录，保留各语言文件名；构建器只读取该最终路径。构建器收据分别记录夹具清单和该主机场景的精确路径、摘要。

[run_frps_ota_interop_test.sh](../firmware/tests/run_frps_ota_interop_test.sh) 在本机回环启动 catalog 精确版本的官方 FRPS，链接冻结 efrp／eota 与 Base 实际 listener、handler、owner 和 V4 收据。双目标完整 signed 输入、成功／写入失败／NVS不确定六场景、真实流预算及替身范围见[FRP 软件检查点](../docs/operations/frp_ota_software_checkpoint.md#官方-frps-与原生-ota-宿主联调检查点)。它不安装生产 FRPS，不访问设备；POSIX／RAM Flash／SDK替身不能授予公网、实板或bootloader资格。

## 百次与连续72小时有限驱动

`native_lifecycle_run.py --help` 列出完整参数。`--mode cycles-100` 固定100次完整周期，`--mode soak-72h` 固定本轮连续259200秒，默认每小时OTA、每5秒业务。运行前由既有流程完成实体UUID／芯片／4MiB／安全状态、本轮恢复基线、唯一租约、正式信任和R5验收；本工具不管理备份／租约，也不读取一份摘要就授予R5通过。

宿主MQTT依赖复用[mqtt-lab-requirements.txt](mqtt-lab-requirements.txt)的固定Paho 2.1.0，安装到仓外私有环境；实际运行和验证须使用这个环境的Python：

```bash
python3 -m venv /absolute/private/native-run-venv
chmod 700 /absolute/private/native-run-venv
/absolute/private/native-run-venv/bin/python -m pip install -r tools/mqtt-lab-requirements.txt
/absolute/private/native-run-venv/bin/python tools/native_lifecycle_run.py --help
```

两份 `--image-a`／`--image-c` 必须是同一已评审原生实现、同目标、不同完整signed摘要且均被外部R5覆盖的冻结镜像，分别显式提供 `--image-*-sha256`、`--image-*-size-bytes`、`--image-*-target`。同镜像OTA由实际收据拒绝，所以不能以一份bin运行百次，也不能把FRPS测试的旧架构A当成本轮已验pair。启动的 `--device-id`、`--initial-boot-id` 和 `--endpoint` 精确绑定本轮设备；当前运行身份不在pair、提交前boot改变或本地文件变化都会停止写入。

`--r5-evidence-file` 为0600原件，必须给出预先核对的 `--r5-evidence-sha256`；`--capacity-log-file` 为0600活动原始日志，绑定本轮起始前缀、inode及结束追加范围，不跨轮拼接。MQTT使用既有 `--credentials-file`／`--business-management-key-file`／`--ca-file` 与严格TLS、QoS1、非retained；FRP key只从 `--frp-management-key-env` 指定的现有环境变量读取，不打印凭据。输出 `--output-directory` 必须是新目录，0700／0600 journal逐条fsync，保存单次run、原operation、精确身份、时刻与中断原因。

每次周期包含最大3924 B认证原生事件、后台上传期间业务、一次FRP固件PUT、新boot原ID成功与完整镜像对账、再次业务及暂停／恢复。PUBACK不是业务成功；必须收到本boot／序号／摘要对应的实际reported结果。上传回调只唤醒有限业务线程，记录业务待决窗口与host上传progress区间的相交，不能据此证明业务执行时刻或设备满合法峰值。丢失结果、unknown、错误身份或非预期重启均保留原ID并停止，不重发写、不换ID。区分保留ID、命令已尝试与设备实际确认准入；尝试边界不证明字节已发出。已尝试操作异常后尽力一次有限原ID只读查询，保留实际响应和独立身份裁决，不覆盖主异常或改判成功。

72小时普通步骤超过10秒宿主进展空档即中断，OTA有界等待单独计时；每秒检查等待并对照墙钟／单调钟增量，任一回退或差值超过固定1秒容限中断，系统睡眠不能绕过Mac单调钟不推进的事实。并发观察在同一现有锁内采样和更新进展／OTA窗口，避免线程调度把正常递增时钟误判为回退；日志与异常处理在锁外。最终R5／容量原件摘要与已绑定前缀核对后，再执行同一宿主观察；正常摘要终点直接使用该次已核验采样，异常保留最近实际记录的原始采样，`observation_end_scope=last_recorded_host_clock_sample`。采样前失败不会伪称取得新时刻，真实回退的负时长保持，已有主中断原因不被收尾异常覆盖。后续摘要／Journal写入不计入观察时段，也不宣称检测了这段时间。OTA窗口仍只是有界等待，允许的等待时间不证明设备瞬时连续状态。两种模式始终输出 `qualified=false`／`r6_passed=false`，只记录是否完成本轮有限观察；全任务回收、连续块峰值、公网脱离Mac、断电和Flash寿命须由外部实板原件共同裁决，短轮次不能相加成为72小时。依赖／凭据／CA等Journal创建前的预检错误只返回CLI失败；Journal建立后的中断才保存run摘要。
