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

有线 OTA 是下发 URL 后由设备 HTTPS 拉取完整 signed bin；软件扫描本地文件得到摘要／长度，提交前核对同设备、当前 boot 与 OTA 可用状态。当前 USB CLI 提交后返回 running／unknown；持久结果须另按原 operation_id 查询，当前镜像身份须独立 `firmware.status` 核对，不能把这些分开的调用当成已自动完成新 boot 成功对账。写只提交一次，超时／unknown不重发、不另建操作。ESP Tool Mac App 的当前职责是本机有线管理，使用这些设备命令，本机调用链独立验收。官方有线刷写／恢复由对应宿主能力执行，不能与 app OTA 混同。

## 原生 MQTT 业务事件

`business_event.py` 从私有普通事件文件生成域隔离 HMAC帧；key 文件须0600、非符号链接，不回显key。`business_event_publish.py` 通过严格 TLS／QoS1／非 retained 仅发布一次，先核对本 boot reported 的下一连续序号，随后按 boot、序号、原始事件摘要和实际 business result 对账。输入只绑定原生业务身份。

完整 event frame 最多4096 B，业务原始字节最多3924 B；新固件输入语义与12字段 reported 见[设备协议](../docs/design/device-protocol.md)。Broker PUBACK不是设备执行成功；丢失结果不自动重发业务或换序号。

## 构建与回归

`build_package.py` 在锁定 SDK 环境中消费同次 Core 快照，构建、签署清单并归档
双目标原生固件。它不分配编号，不连接设备。先保存源码，再冻结完整的已解析
sdkconfig、公开验证键、SDK 派生、依赖锁与工具链；路径保留在仓外输入文件，
包内只保留摘要和工具身份。私钥不进入包或清单。

```bash
python3 tools/build_package.py freeze --target esp32c3 \
  --sdkconfig /absolute/private/sdkconfig \
  --public-key /absolute/private/verification-key.bin \
  --signing-trust software-test --input /absolute/private/firmware-input.json
```

将返回的 `input_manifest_sha256` 作为本 run 的
`FAST_DEPLOY_PROFILE_SNAPSHOT_ID`，再由显式冻结的 Core 按既有持久状态分配
`firmware / esp-base / riscv32 / esp32c3` 或
`firmware / esp-base / xtensa / esp32` 快照。版本必须等于数字分支和
`release.json`；传入完整 `FAST_DEPLOY_*` context、
`FAST_DEPLOY_PACKAGE_CORE_ROOT` 与 `FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE`。
原始 `client-version-result.json` 须与快照在同一目录。不要为此另建计数器，
也不要以本地号 0 封装正式包。

```bash
python3 tools/build_package.py build --input /absolute/private/firmware-input.json \
  --build-dir /absolute/private/firmware-build
python3 tools/build_package.py package --input /absolute/private/firmware-input.json \
  --build-dir /absolute/private/firmware-build --artifact /absolute/private/esp-base.zip
python3 tools/build_package.py verify --artifact /absolute/private/esp-base.zip
```

每个目标使用独立仓外构建目录。入口核对真实 ELF、app 描述、镜像校验和、
公开组件、bootloader、双槽几何、初始 OTA 数据，以及 ESP32 app／bootloader
内嵌公钥。已完成构建和已签清单只回读复用；同 run 重试不重新生成随机 RSA-PSS
签名。ZIP 与收据沿用 Core 的不可覆盖目录归档。

外部签名路径使用 `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`，先 `build`，
将实际 unsigned app 交给签名事实源。ESP32 还须签署实际 unsigned 分区表。
使用 `manifest --signed-image /absolute/signed-app.bin` 生成冻结清单；
ESP32 同时传入 `--signed-partition /absolute/signed-partition.bin`。
清单由相同键、相同官方 espsecure v1／v2 方案签署后，`package` 传入上述
参数及 `--signed-manifest /absolute/signed-manifest.bin`。所有签名必须覆盖
本次原字节，不能用旧签名或重新构建的近似输入替代。

最终 ZIP 包含完整 signed app、bootstrap 文件、ELF、SDK／组件锁、公开验证键、
Core 原始记录与已签清单。它不包含设备配置、NVS 或恢复资料，也不授予刷写
这些 bootstrap 文件的权限。普通 `verify` 证明包内自洽；发布检查必须额外传入
`--require-release --public-key /absolute/authorized-public-key.bin`。
软件测试键必须标为 `software-test`，此模式会被发布检查拒绝。正式键来源、
历史高水位、设备持久防退和旧设备迁移仍分别验收。

```bash
python3 -m unittest discover -s tools -p 'test_build_identity.py' -v
# 使用已导出的锁定 IDF Python／espsecure 环境
python3 -m unittest discover -s tools -p 'test_build_package.py' -v
python3 -m unittest discover -s tools -p 'test_business_event*.py' -v
python3 -m unittest discover -s tools -p 'test_device_control.py' -v
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

当前 schema 2 要求 BEGIN 后唯一 MQTT 观测行，再读取 REGION／TASK／DOMAIN／END；schema 1 仅解析既有冻结原件，同 boot 不允许切换版本。parser 严格核对实例编号、真实所有权上限、请求字节等式、采样 `start <= until <= BEGIN`、固定成本与累计历史；计数失效保留原 flag，禁止授满输入局部观测，独立 SDK 数值门不因此冒充全部 R5。输出的 `maximum_inbound_owner_tuple_observed` 只表示同一次 partial 回调确有三个4096 B完整 owner 加一个4096 B未完成 owner，不证明业务认证、outbox最大合法状态或OTA重叠。`outbox_wire_bytes_at_rx_peak` 是同 SDK API锁期间的协议字节，不含 outbox节点／分配器元数据；原 FULL 只证明该条请求被现有 admission 拒绝。实体重叠必须以同 boot／同 operation 的真实 OTA消费锚点包住整个采样区间，并以严格 TLS peer原 MID／DUP／PUBACK账本独立核对 outbox。解析始终保留全局下一申请、R5和R6为false。

统计成本全部留在新候选：TLSF 每 region 12 B、新 heap 出生字段的真实 padding、trace 每 TCB 8 B、SDK 全局 36 B、32 个存活 TaskStatus／名称缓冲、诊断代码和栈，以及任务终态 HWM 扫描的时间。必须按目标实际 ELF／map 和操作测量报告；任何费用都不加回空闲或栈余量。

独立 `ESP_BASE_FLASH_IO` 行保留 OTA／配置两类短 claim 的 `completed_claim_count`、`acquire_failed_count`、`release_failed_count`、`max_wait_us`、`max_held_upper_bound_us` 和 `counters_valid`。等待计时从本次入口到成功 CAS 后或失败返回前；占有上界从成功那次 CAS 前到真实 release 后，包含该窗口内的观测与调度成本。只对成功 release 的完成 claim 更新占有最大值，失败释放、负／倒退时钟或32位溢出永久令该类历史无效；计数／过长时长饱和，不回绕、不运行中清空。配置提交仍是单次尝试，BUSY 的时长不能代表跨 control pass 的排队时间；配置类也包含启动 NVS／身份／配置读取。OTA 类包含启动核验、收据及 firmware.status 的短 I/O；闲置 scratch 不再被访问，非零计数不能独自证明 MQTT 块与业务在同一阶段重叠。

`ESP_BASE_OTA_PHASE` 的 retire_inactive／prepare／select 和必要失败清理边界使用原 boot／operation／request ID、真实微秒时刻、结果与累计完成计数；候选身份仍从同 boot 的独立 firmware.status 原件绑定。边界时刻包住外层操作，含 begin 行打印开销，不能分拆冒称内部 erase／hash／verify 的精确耗时或逐阶段持续最大 MQTT 状态。聚合计数在 release 后发布，边界差值只代表已发布的完成观测；MQTT 联合覆盖仍须真实块请求、实际写入与 Broker 原始轨迹，不能仅凭异时 maxima 相加或健康控制记录推定同存峰值。

以上行均位于容量 END 外，容量 schema2 字节与 BEGIN 截止不变；每个主动重启前及 OTA 失败持久化后、清理原请求前也保留最终 Flash 快照。观测仅使用48 B固定聚合数据、一个平台短临界锁、8 B OTA 交接时刻以及调用栈临时快照；`aggregate_storage_bytes` 仅为聚合数组与锁的 sizeof，不是全部固件开销。`ESP_BASE_FLASH_IO_OUTPUT` 记录本次快照和两行打印的实测窗口，明确不含它自身末行。所有新增代码、格式化、UART锁／线路、栈及调度成本均留在新候选的实际 ELF／资源／时延测量中，不加回门限，也不称聚合 max 覆盖了全部时间区间或未完成 holder。

```bash
IDF_PATH=/absolute/managed-sdk python3 tools/test_managed_sdk.py
python3 tools/test_sdk_capacity.py --idf-path /absolute/managed-sdk
```

前者先实际核对 SDK，再以隔离夹具验证拒绝边界；后者直接编译受管 SDK 的 TLSF 和实际任务最终采集片段，包含 100000 次碎片化操作、逐稳定状态 walk oracle 与移动 realloc 共存反例。host 及 sanitizer 结果不替代 MCU、正式签名或双板容量验收。

正式诊断使用既有 stdout／VFS，不直接使用缺少源码超时保证且不支持 long long 的 ROM formatter。C3 保持 USB Serial/JTAG no-driver 路径：VFS 在无连接时返回 EIO，FIFO 无 host 读取时按最后成功输出起 50 ms 后丢字节；完整帧仍须由原始日志确认。ESP32 的 VFS 绑定锁定 IDF UART 驱动，以 IRAM ISR 和 9,217 B RX ring 接收输出期间到达的请求，TX buffer／event queue 均为 0，不新增任务；初始化与失败回滚见[固件说明](../firmware/README.md)。UART 按实际字符数和 115200／8N1 计线路费用，不等待 host ACK；驱动、stdio 锁、USB 慢读和 MCU 调度成本仍须实测，不以线路预算冒充最坏时延。RX ring 与 ISR 的实际内存成本纳入同候选容量验收，丢帧或丢尾不得补成覆盖。

```bash
python3 tools/analyze_capacity.py --target esp32 \
  --boot-id <本轮bootUUID> --sdk-lock-sha256 <冻结sdk-lock文件SHA256> \
  --uart-log /absolute/private/raw-uart.log --json
python3 -m unittest discover -s tools/tests -p 'test_analyze_capacity.py' -v
```

解析只接受完整原始 UART 行，绑定 boot／SDK lock、帧序号、region 和任务实例，复算保守下界并明确截至 BEGIN；不会接受文本前缀、跨文件残帧、未知字段、坏计数或最低值回退。任务生命周期只以最近两个完整快照为锚点，正常不完整快照不据缺失实例推断退休，也不丢弃原始日志。定向软件回归与真实 producer 输出组合不授予真实下一申请、R5／R6 或 MCU时延资格。

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

parser 固定并核对当前 sdk-lock、正式 producer 和两份受管 patch 的原件摘要，不接收调用方公式或任意 `maximum_verified`／`coverage_complete` 声明。当前 recipe 支持无 owner／poison、non-EXEC、无硬件 caps 改写的 `32BIT`／`8BIT`／`INTERNAL`／`DEFAULT`／`IRAM_8BIT` 完整掩码组合。双目标指针和 size_t 为 4 B，TLSF minimum block 为 12 B、block header 为 16 B：普通请求先取 `adjust=max(12, align_up(size, 4))`；alignment 低于 4 B 时按 4 B；高于 4 B 时使用真实 `tlsf_memalign_offs` 的 `search_size=align_up(adjust+alignment+16, alignment)`。calloc 应提供乘积已核对的总字节数，移动 realloc 应提供新块实际申请；并存旧块费用仍由独立生命周期证据覆盖。

同一锁定 recipe 另支持 ESP32-C3／ESP32 的三个真实完整 DMA 掩码：`DMA=8`、`DMA|INTERNAL=2056`、`DMA|INTERNAL|8BIT=2060`，仅限 caller alignment 为 1、2、4 B。两目标内部 cache alignment 为 0，这些请求不含 SPIRAM／EXEC／descriptor／cache／SIMD 位，实际 `esp_heap_adjust_alignment_to_hw` 保持完整 caps，之后经普通 `multi_heap_malloc` 和 TLSF；有效对齐仍为 4 B。筛选保留 DMA 位，不借用非 DMA 或晚出生 region 的下界。caller alignment 大于 4 B 时硬件层会先圆整请求 size，不能套用上述 plain memalign 公式，仍报告 `unknown`；其他 DMA 组合、EXEC alias、cache、descriptor、SIMD、其他未建模路径和目标整数溢出也继续 `unknown`。实际 AES 临时输入／输出各最多 1600 B，DMA 请求 alignment=1；PSA SHA 上下文使用完整 `DMA|INTERNAL`，不能因普通内部堆充足改用更弱 caps。

`python3 tools/test_dma_request_capacity.py --idf-path /absolute/managed-sdk --compile-commands /absolute/build-c3/compile_commands.json --compile-commands /absolute/build-esp32/compile_commands.json` 从两目标真实编译输入提取硬件归一化与 cache 函数，交叉编译核对目标 TLSF 的指针／size_t／header／minimum block／alignment，并用 ASan／UBSan 验证真实 host TLSF 碎片化状态中的充分条件。host allocator 仍使用宿主 ABI，实际申请成功比较限至少24B的请求；MCU的12B minimum由锁定源码与目标交叉编译证明，不把宿主64bit布局当MCU布局。可重复提供 `--compile-commands` 核对多个候选；`--json` 仅输出机器摘要。该测试不访问设备，也不授实板资格。

每项从最近完整任务帧的原始 REGION 重新匹配全部 caps，三优先级求并集，一个物理 region 只计一次，晚出生 region 不贡献从启动开始的下界。只有其 `max(region min_largest)` 覆盖 search-size 才输出 `status=fit`；下界不足输出 `unknown`，不推断实际申请必然失败。结果包含原始 size／caps／alignment、search-size、总空闲与连续申请下界、margin、最弱 margin／请求 id 和可证 BEGIN 截止。不完整尾帧不能延长请求证明。`fit` 仅表示该单个已提供请求在锁定 recipe 与已读历史下界下的充分条件，不承诺并发后的未来成功。

结果保存在 `provided_request_fit`，全局 `next_maximum_legal_request_verified`、`r5_qualified`、`r6_qualified` 始终为 false；`next_request_unclosed_reasons` 明示全合法消费者集合、动态真实输入、并发／realloc 生命周期和固件配置源码消费绑定仍未封闭。CLI 退出码仍只反映输入有效性与三个数值门；调用方须读取各请求 status，不得将退出码 0、`all_provided_requests_fit` 或一份 sidecar 当作全局资格。

本产品 HTTPS OTA 头申请按 `firmware/CMakeLists.txt` 的 `HTTP_MAX_HEADER_SIZE=8192`、受管 OTA 的 RX=1024B／SDK 默认 TX=512B，以及本次正式 `SAVE_RESPONSE_HEADERS=n`／`ENABLE_GET_CONTENT_RANGE=n` 核算。单个合法响应的 key/value 或累计 Location／认证头申请保守上界为8193B（含 NUL）；拒绝路径的 CR 回调可能先于本接收片的最终计数检查，且完成首个头后仍消费本片剩余数据，故具体请求证据采用更保守的9217B，普通4B对齐 search-size为9220B。HTTP 头的 libc `calloc/realloc` 经 `heap_caps_*_default`，本轮双目标关闭 SPIRAM 后的完整掩码为 `DEFAULT|INTERNAL=6144`；MQTT official RX／TX 与 outbox wire 则直接使用 `heap_caps_*` 和 `MQTT_BUFFER_MEMORY/MQTT_OUTBOX_MEMORY=DEFAULT=4096`，须按真实调用区分。ESP32 的协议与 emqtt 自有工作对象明确使用 `INTERNAL|IRAM_8BIT=10240`，也不能套用 libc caps。不能将旧80KiB头申请或新上界移给未采用此产品编译宏的旧固件。

令 `B=8192+1024=9216`：当前 key/value 与累计 Location／认证头合计请求最多 `2B+4=18436B`；强制移动 realloc 时保留旧块，字符串合计保守上界 `3B+5=27653B`。含 RX、TX、首个完成头所在接收片的缓存正文及缓存移动过程，整条头/缓存链的请求字节保守上界分别为20996B（稳定同存）与30213B（移动峰值）。这些是申请字节上界，不包含 client/transport/URL、TLS、各块分配器开销或其他任务；完整 R5 仍需实际并存峰值、所有消费者与配置绑定，单次 fit 不代替它。

运行 `python3 tools/test_http_header_budget.py --idf-path /absolute/managed-sdk`：先核对 SDK，直接抽取实际 append、header/body callback、fetch 和 cleanup，并编译真实 parser，以 sanitizer 核对分片、相邻接受/拒绝边界、长 field/value、重复/折行辅助头、1xx 后同片消费、缓存正文、移动 realloc 与低内存失败清理。transport、事件与平台对象使用宿主替身；测试验证当前正式关闭上述两项功能的头/缓存分配链，不验证 TLS、实体时延或整个 SDK 初始化。它不访问网络或设备，不授实板 R5/R6；双目标 reconfigure 与 signed 构建输入仍须分别核对。

## 一次性有线迁入与历史输入

`preflight_v3_migration.py` 仍只做旧C3 v1/v2配置的只读预检，不是当前原生布局迁入。`archive_esp32_at.py` 核对两份完整备份，按 NVS 前三页和 `at_customize` 前两页生成 20 KiB 原字节归档；两分区余尾必须全 FF，并逐字节重建完整原分区，不解码或打印凭据。ESP32 候选将其放入只读 `at_old_raw@0x3e5000/0x5000`。旧 `esp32_product` 的四页输入仅在一次性离线准备时补入第三 NVS 的 FF 页，运行时只消费当前五页布局。新布局的一次性离线准备工具只在审计原终态、归档回读与完整输入验证后生成候选，不触达设备；未决／损坏保持阻断，不能清空NVS。最终精确命令与软件边界见[原生软件检查点](../docs/operations/native_software_checkpoint.md)。

旧 AT 空 Wi-Fi 判断先核对 blob 类型与旧 SDK 的 36／65 B 字段长度，再只核对声明 payload 是否全 FF；NVS 槽尾 padding 不属于凭据，但仍随五页归档原样保留。有效 payload 的非 FF 字节、类型／长度不符或 CRC 损坏均阻断。

原生候选／公钥／分区输入、旧C3双备份和现役AT双备份的读取先以非阻塞方式打开，再核对普通文件及原权限／尺寸规则；无写入方的FIFO会明确拒绝，不会停在打开阶段。软件入口和原始证据追溯见[验证说明](../docs/operations/native_software_checkpoint.md#固定输入与证据)；本工具仍只准备离线输入，不能代替本轮实体身份、正式信任、恢复基线和唯一租约。

`prepare_native_layout.py --source-layout c3_mqtt_factory` 用于独立 MQTT 实验固件首次迁入原生 Base：仅接受 C3 的 `nvs@0x9000/0x6000`、`phy_init@0xf000/0x1000`、`factory@0x10000/0x100000` 三分区，核对完整未签名 `esp_mqtt_broker_client` 镜像、checksum／摘要和擦除态槽尾。NVS 与 `0x3e0000` 后的旧 Base 持久范围必须全为空；有数据则阻断，不能据此清空旧身份或配置。来源没有 Base UUID，只记录本轮独立读取的规范 `--source-efuse-mac`，实际物理绑定须另核对双份恢复基线和写前 ROM 身份。原 factory app 占用的新 otadata／PHY／coredump 区在候选中重新装配，原始完整字节先归档并读回。新 UUID 由首次启动的设备生成，Wi-Fi 经当前 USB 配置链设置；该离线工具不连接或写入设备，也不授予正式交付资格。

旧动态包生命周期、专属NVS容量探针与旧QEMU组合入口已删除。当前离线准备只消费明确SDK、真实旧布局终态与签名固件，工具与本轮设备写入资格分别核对。

## 资源与独立 MQTT 实验

`capacity_observation.py` 从实验UART读取顺序周期堆／完整任务快照／退出记录，保持16384／24576／1024 B门；必须匹配真实观察器的完整启动声明、明确 target、唯一启动及非递减 uptime。缺失、错目标、重复启动、uptime 回退、损坏或不完整数据不通过，跨文件也不合并不同启动轮。原日志只读并保存摘要。它不是瞬时峰值或完整native生命周期资格，观察器成本不加回。

旧 `prepare_capacity_observer.py` 及 `LAB_ONLY` 5 秒观察仅解释已冻结历史副本；当前正式源码拒绝 `ESP_BASE_CAPACITY_OBSERVER_LAB`，不要从活动 canonical 再生成该副本。`capacity_observation.py` 可离线解析旧日志，始终不授完整峰值或生产资格。旧 pre-deletion／存活采样、64 条退出缓存、日志丢失及异常重启的边界仍保留，不能把旧数值移给正式统计新候选。原件不改写，也不跨日志或 boot 拼接。

`mqtt_lab_check.py`／`mqtt_resource_report.py` 继续用于独立MQTT实验的严格TLS、往返、计数／栈和回收。实验输入必须位于仓外，不混入普通Base，独立实验不替代本轮MQTT／原生业务／OTA同存验收。
