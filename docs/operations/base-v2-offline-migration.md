# ESP Base v1→v2 配置离线预检与迁移边界

## 当前事实

本仓固定 ESP-IDF 提交 `855937cf9dcee13ee9c423fb0319238cdc8d53fd`，4 MiB 分区表中默认 `nvs` 位于 `0x9000/0x6000`，`otadata` 位于 `0xf000/0x2000`，`ota_0`、`ota_1` 分别位于 `0x20000/0x1e0000`、`0x200000/0x1e0000`，`base_store` 位于 `0x3e0000/0x20000`。v1 配置是 `base_store/base_config/committed` 的 112 字节 `EBCF` v1 blob；可选 OTA 收据是 `base_store/base_ota/operation` 的 118 字节 `EOTA` v1 blob。默认 `nvs/base_identity/device_uuid` 为独立持久身份。

v2 候选仅将已有 Wi-Fi 原值转为同一个 `committed` 键：24 字节 `EBCF` v2 header 后紧接原 SSID 和 Wi-Fi 密码；`revision` 原样保留，MQTT flag 与 hostname、username、MQTT password、CA、port、HMAC key 均为空。MQTT 凭据和管理 key 不在离线环节生成或注入；v2 首启后由物理 USB `config.set` 完整首配并按正常 CAS 语义推进 revision。

## 只读预检与候选生成

设备工具先在本轮重新确认唯一 ESP32-C3、4 MiB、设备 UUID 和传输端点，取得**两次独立读取**的完整 Flash 恢复件，保存于仓外私有目录。此脚本只接受当前用户拥有、权限 0600 或更严的两个不同普通文件，每份必须恰为 `0x400000` 字节。恢复件、候选和命令行中的私有目录不进入 Git、普通日志或发布制品。

在固定 SDK 的 Python 环境中运行：

```bash
python3 tools/preflight_v2_migration.py \
  --backup-a <仓外第一份完整Flash备份> \
  --backup-b <仓外第二份完整Flash备份> \
  --idf-path "$IDF_PATH" \
  --device-id <本轮独立核对的设备UUID>
```

选择 `--output-base-store <仓外未存在的目标文件>` 时，工具还使用当前固定 SDK 调用的官方 `esp-idf-nvs-partition-gen==0.1.9` 创建**单独的** `0x20000` 字节候选分区，权限 0600；候选不包含其他 Flash 分区，也不接线任何写设备命令。输入 NVS 有可选 OTA 收据时，候选保留其完整 118 字节原值。脚本用固定 SDK 的 NVS 解析器重新读取候选，要求键集合、类型和 blob 字节与期望逐项相等；输出完整 Flash 及候选 SHA-256，不输出身份值、Wi-Fi 凭据或 OTA 收据内容。

只读预检逐字节比较两份完整 Flash，按本仓 CSV 通过固定 SDK 生成分区表并逐字节核对；检查 `otadata` 两副本的序列、CRC、状态和选槽，报告两应用槽的完整槽 SHA-256 与空槽／镜像头事实；检查 NVS 页、已写条目和 blob 数据 CRC，枚举当前有效的 namespace/key/type/value 长度。仅允许默认 `nvs/base_identity/device_uuid` 与 `base_store/base_config/committed`、`base_store/base_ota/operation`；身份、v1 编码或 OTA 收据不规范、其他活动键、未知或加密 NVS、CRC 异常、分区不符、非 VALID 选槽，均直接阻断。旧配置键缺失时依 v1 固件的实际读取合同视为默认空配置，再生成 24 字节空 v2 blob。

测试使用固定 SDK 官方 NVS 生成器创建假件，覆盖双备份差异、未知键、CRC、v2 误读、pending 选槽、Wi-Fi 与 OTA 收据候选读回：

```bash
IDF_PATH=<固定SDK路径> python3 tools/test_preflight_v2_migration.py
```

官方 `nvs_partition_gen.py` 是**从 CSV 新建 NVS 分区**，没有在旧 NVS 镜像上原位更新单键的 API。生成候选会改变 NVS 页历史、空闲布局等物理字节；可证明的是当前白名单内的**活动逻辑记录**逐键相等，以及对整片其他分区未创建任何替换字节。只读预检和候选镜像不能证明应用可启动、签名有效、物理 Flash 可读写、掉电恢复或真实运行槽。不能把镜像头/全槽摘要当成完整 signed bin 的校验结果。

## 一次性维护镜像的 NVS API 合同

若采用设备上原位改写，先在两份完整 Flash 恢复件、设备身份、当前 `otadata` 与双槽镜像均验证后，按受控流程使**两个可能启动的应用槽都不再含 v1-only 固件**；一次性维护镜像须先本地启动并确认 VALID，另一个槽也须为能读取转换前后配置的维护镜像，才允许触及 `base_store`。转换后不能让旧 v1-only 槽成为自动回滚目标。此维护镜像只用于停机转换，不进入普通产品固件或长期兼容合同。

维护镜像通过固定 SDK 的 `nvs_flash_init_partition("base_store")`、`nvs_entry_find`/`nvs_entry_info` 枚举确切记录；先读取 `base_config/committed` 的 112 字节 v1 blob，或按旧固件语义识别缺失；完整读取可选 `base_ota/operation` 并记录原字节。遇未知键、非 v1 值、NVS 初始化错误、receipt 不确定、运行槽未确认 VALID 或可回退槽不安全即停机，不擦除分区。输入经同一 v1 编码验证后生成 Wi-Fi 原值 v2 blob，revision 不变、MQTT absent；使用 `nvs_open_from_partition`、`nvs_set_blob("committed", ...)`、`nvs_commit` 写入同一个键，再重新打开并按 v2 codec 精确读回，重新枚举键集，并将 OTA 收据及默认身份逐字节读回比较。写入开始后的任何错误均为存储状态未知，只读回真实状态，不自动重试或复位。

也可在停机维护窗口审查上述**单独 `base_store` 候选**，但只读工具没有执行分区写入、选槽或恢复。无论采用哪一种受控写入路径，必须先保存可实际恢复的完整 Flash 基线，完成同键 v1→v2 改写和读回核对，再使两个应用槽均为 v2-only 且当前选择器/运行槽安全，才允许首次 v2-only 启动。改写前和改写后分别核对默认 `nvs` 身份、可选 OTA 收据、分区表与其他未授权修改的分区；不能用旧 v1 自动回滚来补救已经写入的 v2 NVS。

本仓当前只交付离线预检和候选字节证明；维护镜像、设备写入、真实双槽启动及恢复仍须后续独立实现与设备授权。
