# ESP Base v1/v2→v3 配置离线预检与迁移边界

## 当前事实

现有实板仍运行 v1 配置，`base_store/base_config/committed` 为可选的 112 字节 `EBCF` v1 blob；v2 只在此前软件候选中实现，并未完成实板部署。普通新固件只读 `EBCF` v3，遇到 v1 或 v2 会停止启动并保留 NVS，不会自动转换、擦除或生成凭据。分区仍为 4 MiB：默认 `nvs` 位于 `0x9000/0x6000`，`otadata` 位于 `0xf000/0x2000`，`ota_0`、`ota_1` 分别位于 `0x20000/0x1e0000`、`0x200000/0x1e0000`，`base_store` 位于 `0x3e0000/0x20000`。

v3 候选只改同一个 `base_config/committed` 键，保留 revision 与原字段：v1 输入转为 40 字节 header 加原 Wi-Fi 字节，MQTT/FRP 未配置；v2 输入在严格校验后保留原 Wi-Fi、MQTT 凭据和管理 HMAC key 字节，FRP 未配置。可选 `base_ota/operation` 收据保持原 blob。候选不生成或更换任何 Token、key、密码、证书，也不替换默认 `nvs/base_identity/device_uuid`。

## 只读预检与候选

设备工具须在本轮重新确认唯一 ESP32-C3、4 MiB、设备 UUID 和传输端点，获取两次独立读取的完整 Flash 恢复件，保存在仓外私有目录。输入须为当前用户拥有、权限 0600 或更严的两个不同普通文件，且每份恰为 `0x400000` 字节。在固定 SDK 的 Python 环境运行：

```bash
python3 tools/preflight_v3_migration.py \
  --backup-a <仓外第一份完整Flash备份> \
  --backup-b <仓外第二份完整Flash备份> \
  --idf-path "$IDF_PATH" \
  --device-id <本轮独立核对的设备UUID>
```

可加 `--output-base-store <仓外未存在的目标文件>`，生成权限 0600 的独立 `0x20000` 字节 v3 候选分区镜像。脚本使用固定 SDK 官方 NVS generator 生成候选，再以官方 NVS parser 逐键读回，验证精确键集、类型与字节；输出完整 Flash 和候选 SHA-256，不输出身份值、Wi-Fi、MQTT 凭据或 OTA 收据内容。脚本不打开串口，不执行 Flash/NVS 写入。

预检先逐字节比较两份 Flash，核对本仓固定分区表、otadata 选择器和双槽头/全槽摘要；NVS 仅允许默认身份、同键配置与可选 OTA 收据。非规范 v1/v2、未知/重复键、NVS CRC 错误、加密 NVS、身份不符、未决或非 VALID 选槽均阻断。镜像头与全槽摘要仍不证明固件可启动或签名有效。候选使用官方生成器新建 NVS 分区，会改变页历史与空闲布局；证明范围是白名单内活动记录及目标配置值，不是其他 Flash 字节无差异。

测试入口：

```bash
IDF_PATH=<固定SDK路径> python3 tools/test_preflight_v3_migration.py
```

## 首次启动与一次性写入边界

本仓只交付离线只读预检和候选字节证明，不提供设备写入或选槽命令。首次 v3-only 启动前，必须保全两份可恢复的完整 Flash 基线、确认身份与真实 otadata，先使两个可能启动的应用槽都具备读取 v3 的能力，完成同键配置转换和精确读回，再确认启动槽及可回退槽安全。不能让 v1/v2-only 镜像成为 v3 NVS 的自动回滚目标。

若采用一次性维护镜像原位写入，维护镜像需先安全启动并确认 VALID，再使用固定 SDK NVS API 枚举白名单记录、验证原 v1/v2 blob、生成 v3 同键值、`nvs_set_blob`/`nvs_commit` 并重新打开逐字节读回。开始写入后的异常都视为状态未知，只读取真实持久状态，不自动重试、擦除分区或复位。还须读回默认身份与 OTA 收据，核对未授权修改的其他分区。该维护实现和真实双槽迁移仍未完成；没有精确设备授权与恢复基线时，不得执行物理写入。
