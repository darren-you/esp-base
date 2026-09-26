# C3 NVS 同键离线探针

这个独立 ESP-IDF 项目只用于 QEMU 内的 `base_store` 复制件。源码不含私有 Flash、设备身份或凭据；不接入 Base 产品应用，也不提供设备烧录流程。实际输入必须从同一设备的两份一致完整 Flash 备份读取，并保存在仓外权限受限目录。

## 仿真步骤

1. 核对 [SDK 锁](../../../sdk-lock.json)和 [C3 分区表](../../partitions/partition_table.csv)。在仓外把 `base_store@0x3e0000/0x20000` 从已核对的完整备份复制到权限 0600 的文件；不要在命令输出中显示内容或 SHA。
2. 在固定 SDK 环境用绝对 `-B` build 路径和独立 `SDKCONFIG`，分别构建 `-D PROBE_INIT_ONLY=ON`、默认提交模式、`-D PROBE_VERIFY_ONLY=ON`。三个构建均使用此项目根目录的 CMake 与目标专属 defaults。
3. 为每种模式创建仓外 4 MiB QEMU Flash 副本，填充 `0xff`，按构建的 `flasher_args.json` 放入 bootloader、分区表、初始 otadata 和该模式的 app，再把私有 `base_store` 放在 `0x3e0000`；禁止将副本、完整串口日志或敏感分区写进 Git。
4. 用固定的 Espressif C3 QEMU `-nographic -machine esp32c3 -drive file=<仓外镜像>,if=mtd,format=raw` 分别运行初始化与提交模式。每次运行后逐字节比较源 `base_store` 的 32 个 4 KiB 页；日志只提取 `PROBE_` 结果。提交后用验证模式的新进程打开已修改的 QEMU 镜像，确认 `PROBE_V3=1`，再比较所有 32 页是否出现额外改动。
5. 用固定 SDK 官方 NVS parser 只对第 0 页加内存中的空白尾页做活动记录核对，并用 `tools/preflight_v3_migration.py` 的 `convert_v1_wifi_only` 比较转换值；真实后 31 页若仍无效，正式预检必须继续拒绝，不能用这个仿真放行设备写入。

构建命令形态如下，`probe_work_dir` 必须改成仓库之外由当前用户独占的绝对路径：

```bash
probe_work_dir=/absolute/private/path
source "$IDF_PATH/export.sh"
python3 ../../../tools/check_sdk.py --path "$IDF_PATH"
idf.py -C . -B "$probe_work_dir/build-init" -D SDKCONFIG="$probe_work_dir/sdkconfig-init" -D PROBE_INIT_ONLY=ON build
idf.py -C . -B "$probe_work_dir/build-commit" -D SDKCONFIG="$probe_work_dir/sdkconfig-commit" build
idf.py -C . -B "$probe_work_dir/build-verify" -D SDKCONFIG="$probe_work_dir/sdkconfig-verify" -D PROBE_VERIFY_ONLY=ON build
```

以上命令从本探针项目根目录执行；三个 build 和 sdkconfig 路径各自独立。普通 Base 的生成配置还须独立核对 NVS 相关选项，本轮结果以离线迁移记录的固定输入为准。

本探针只测试固定 SDK 在 QEMU Flash 副本上的单次正常路径。异常页来源、断电中断、真实 Flash 时序、旧 bootloader/新双槽启动与完整设备恢复均未由此验证。不要执行 `idf.py flash`。
