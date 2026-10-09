# ota_operation

Base 自有的固件 OTA 约束、持久操作收据及只读签名固件集合观察。两个 target 都固定项目 `esp_base`，双 `0x1e0000` app 槽分别位于 `0x20000`、`0x200000`。C3 使用 `esp32c3/esp_base` 与 RSA v2；ESP32 使用 `esp32/esp_base` 与 ECDSA v1。请求不能修改芯片、签名方案、分区几何或期限。业务是原生固件代码，OTA 不再登记或下载动态业务包。

`device_protocol` 的 USB/MQTT `ota.start` 提供 HTTPS URL；设备 FRP 公网入口登记同一固件意图，再通过已认证的上传连接流式收取完整 signed bin。`inbound_stream` 只是内存来源标志，不属于 wire 字段。两条入口共用唯一升级事务 owner，写前完成静态请求校验、已确认固件集合复核及持久收据 commit/逐字节读回。收到相同操作 ID 不重新下载或重新创建任务；来源等待、下载、选槽和启动确认期间，另一入口不能获得升级事务。

## V4 收据与恢复

唯一收据仍在 `base_store/base_ota/operation`，为固定 182 字节 `EOTA` V4：状态、A/C 物理 subtype、失败码、保留字节、C 完整签名长度、设备 UUID、原操作 UUID、C 摘要、A 摘要和原备用 B 摘要。备用 B 不存在或两个槽属于同一签名身份时，其摘要为零。删除了 V3 中包模式、包摘要、代表事件及 ECS2 sequence。V1/V2/V3、错误长度、保留位非零、损坏或读取失败均返回存储不确定，不能当成键缺失，也不能用清空 NVS 继续启动。

登记时重新观察物理槽与 `CONFIRMED` 签名固件集合；A 必须已 `VALID` 且也是 boot selector。C 与 A 同一完整签名摘要会在收据写入和退役 B 之前被拒绝。只有可证实终态收据可以被新操作替换；未决收据阻断新操作。

应用写入前先按原收据退役确切 inactive 槽。失败后只有未改 app，或完成原意图限定的 inactive 物理清理、来源 A 验签/摘要和选槽核对，才能持久写 `FAILED`；读回不确定仍为 unknown。FRP 上传在等待连接时取消不会先擦槽；完整 prepare 后取消也在 select 前被复核，清理候选 C 后保留原 ID 失败终态。已选择 pending C 的进度不能被当作失败清理目标。

上传 worker 完成时只返回准备进度 `running/202` 或尚未形成持久结果的 `unknown/200`，`result` 为 null。失败提交和精确读回由控制 owner 随后执行，提交证实后才形成 `failed`；上传响应不能提前声称终态。宿主收到上传 unknown、断链或任何未经原 ID 收据证实的上传状态时，继续查询同一 `operation_id`，不重发上传或另起操作。

C 的启动确认独立于 Broker、FRPS 和业务消息：本地 NVS、身份、安全状态、配置和控制任务自检健康满 30 秒，并经过窗口边界后的下一次本地检查，再调用 IDF 确认和读回同槽 `VALID`。需复核 C 完整签名身份与原收据、A 回滚身份及集合稳定性；成功收据 commit/读回完成前保持升级门禁。A 侧中断恢复先核对来源、原 ID 与精确 inactive 槽，完成清理后写失败终态。缺收据只允许首次有线装配产生的已确认 `VALID` 签名基座；无原收据的 pending 固件被拒绝。

`ota.result` 查询原 ID，返回固件 SHA-256、完整长度、target 和目标槽。活跃 worker 或原收据限定的 pending C 为 running；C 已 `VALID`、签名与摘要匹配且 `SUCCEEDED` 持久终态存在才 succeeded。失败须有真实已确认 A 与来源摘要匹配的 `FAILED` 终态。VALID、INVALID、ABORTED 等 otadata 状态单独不能证明升级结果，其他不确定状态返回 unknown，查询不改收据或选槽。

## 签名集合与 Flash 所有权

`esp_base_ota_observe_firmware_set` 只读观察 `CONFIRMED`、`PENDING_TRIAL` 或显式 `PREPARED_CANDIDATE`。运行槽和 boot selector 必须相同；每个涉及镜像都经 `eota_sha256_verified_image` 验签，再核对 Base 项目、芯片、完整长度和分区几何。同一 signed bin 摘要合并为同一固件身份。没有另一受管固件时，IDF 必须明确拒绝该镜像且 inactive 首字节为 `0xff`，才能报告单固件集合。prepared 只允许本次 `eota_prepare` 的精确收据，选槽前再次证明 A/C 完整身份和稳定状态。

通用 HTTPS/stream 收取、镜像头/完整摘要、IDF 验签、槽观察、退役、确认和 rollback 由锁定 `esp-ota` 组件提供。唯一 `esp_base_storage_owner` 升级事务跨任务转交，网络等待期间保留事务 owner；app/otadata、配置/身份 NVS、操作收据与 FRP scratch 的实际物理访问共用独立短时 Flash I/O owner，网络等待不持有此短 claim。该串行化不替代实体 Flash 最坏时延、堆栈容量和寿命验证。

## 首次有线布局迁入

应用 OTA 不修改分区表。一次性离线工具 [prepare_native_layout.py](../../../tools/prepare_native_layout.py) 从两份独立、相同的私有 4 MiB 新鲜恢复件生成仓外完整候选，使用固定 SDK 的官方分区/NVS 工具和 `espsecure` 验签。输入还需本轮独立核对的 Base UUID，或旧 ESP-AT 的 eFuse MAC；旧/新固件均绑定明确目标与验签公钥。工具不打开串口、不操作 eFuse、不写设备。

支持精确 `c3_product`、`esp32_product` 旧包布局、保留的 `c3_v1` 配置布局，以及已核对空 Wi-Fi 的 `esp32_at`。其他布局、未知 NVS 键、重复记录/CRC 失效、加密分区、otadata NEW/PENDING、EOTA PREPARED、ECS2 写入/试运行/中断和未决产品账本均阻断。旧 v1/v2 配置只在保留的 C3 一次性路径转换为 V3，UUID、revision 和已配置字段保留；现役 V3 配置原 blob 字节保留。

旧 V3 终态需与真实 VALID 双槽签名身份、长度、ECS2 绑定、包原字节和账本终态共同核对。工具先把完整原 Flash、原操作 ID、V3/ECS2/账本原 blob 和核对结果写入 0700 仓外目录内的 0600 文件并精确读回，再生成退役旧操作/包键的候选 NVS。身份、配置、revision、凭据及仍活动 SDK 记录保留；默认 NVS、PHY、诊断区与现役 scratch 保持原字节。旧 ESP32 四页 AT 归档只在一次性离线准备中于两页 NVS 后补入一页 FF，再逐字节保留两页 `at_customize`，写入当前只读 `at_old_raw@0x3e5000/0x5000`；无固件运行时旧布局解析。旧 C3 的 `base_store@0x3e0000` 整体归档后搬到新位置，覆盖到新 scratch 的原旧 NVS 不作为现役 scratch 内容保留。候选装入两个同信任新签名 app，并生成两个 VALID 选槽记录；选择 `ota_1`。候选 NVS 的页历史会重建，证明范围是仍活动记录的精确类型和值。

普通应用 OTA 在原 ID 下保留可核对持久结果；**首次有线更换布局与固件身份的旧 ID 此后在设备上返回 unknown**，历史终态从私有 `migration-receipt.json` 与 `source-flash.bin` 核对。不能把旧 ID 改绑新镜像摘要冒充原升级成功。候选删除运行时键是已证实旧终态的明确退役，PREPARED、损坏或未知记录不允许删除。

旧 ESP-AT 没有 Base UUID，工具不从 MAC 生成身份。旧 NVS 前三页与 `at_customize` 前两页按原字节组成 20 KiB 归档并放入 `at_old_raw`；两分区其余尾页必须全 FF，按三页加两页重建后逐字节核对完整原分区。有非空尾页或非空旧 Wi-Fi 字段时拒绝。新 Base 的身份在首次真实启动按既有 UUIDv4 路径产生，配置通过获授权的物理 `config.set` 提供；这不是旧 AT 配置或身份映射。

示例仅生成软件候选，须在固定 SDK Python 环境执行，所有路径都指向已审核的仓外输入：

```bash
python tools/prepare_native_layout.py \
  --source-layout c3_product \
  --backup-a <第一份新鲜完整Flash> --backup-b <第二份新鲜完整Flash> \
  --idf-path "$IDF_PATH" --device-id <本轮已核对UUID> \
  --source-verification-key <旧运行链验签公钥> \
  --app <新完整签名esp_base.bin> --bootloader <新bootloader.bin> \
  --partition-table <新目标分区表.bin> --verification-key <新验签公钥> \
  --output-directory <0700仓外父目录中的尚未存在目录>
```

ESP32 使用 `esp32_product`、ECDSA v1 的 64 字节官方导出验签公钥及已签名分区表，公钥须同时嵌入新 app/bootloader。验签使用本轮私有稳定公钥快照，归档摘要绑定同一份输入。旧 AT 使用 `esp32_at --source-efuse-mac <本轮独立核对MAC>`，不提供 `--device-id`。C3 v1 未签名来源可不提供旧验签公钥，但完整镜像 checksum/hash、产品和芯片检查仍执行，镜像尾后的整个槽必须为擦除字节；有签名或未知尾数据时不能借此跳过旧验签。

后续实板窗口必须重新枚举并独占 USB，核对物理芯片、4 MiB、安全/eFuse 状态、源 UUID/MAC 与双恢复件，使用官方有线刷写链完成完整候选写入及全片读回，再验证签名基座、UUID/revision/配置、双槽和恢复。写入或读回不确定时停止自动重试、保留现场，以同板完整恢复件完成独立恢复验收。工具输出明确标记 `software_only`、`device_verified=false`、`hardware_write_authorized=false`；离线候选及测试签名不代表实板授权、首次启动或生产迁入完成。

软件回归入口：固定 SDK 环境下运行 `python -m unittest discover -s tools -p 'test_prepare_native_layout.py' -v`，另有双目标 host 的收据、启动和串行 owner 回归。合成输入、宿主替身及编译不能代替双目标实体断电、恢复、容量和 72 小时长稳验收。
