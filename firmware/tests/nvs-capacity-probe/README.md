# C3 八页 NVS 容量探针

此独立 ESP-IDF 项目只使用合成数据和仓外 QEMU Flash。它把最大 7,618 字节 Base v3 配置、118 字节 OTA 收据形态的 blob、288 字节 Container 占用 blob 放进同一个 `base_store` NVS 分区，使用固定 SDK 的真实 NVS API 反复提交、重启回读。它不接入产品固件，不读取设备备份，不提供刷板命令。

合成分区表中的 `base_store@0x138000/0x8000` 仅提供连续八页的容量实验。产品分区表未修改。Base 配置使用正式 `esp_base_remote_config_commit_verified` 和规范编码；OTA 使用产品实际 `base_ota/operation` 键及编码布局，但直接执行 SDK `nvs_set_blob`，没有调用 OTA 注册策略；Container 的产品 namespace/key 尚未冻结，本探针用 `base_container/state` 测试 288 字节占用，不验证 Container metadata 语义。每代配置管理 key、OTA 摘要字段和 Container blob 均变化，防止重复值掩盖写入量。

## 复现

需要 [sdk-lock.json](../../../sdk-lock.json) 中的 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，以及支持 `-machine esp32c3` 的 Espressif QEMU。`CMakeLists.txt` 会运行 `tools/check_sdk.py`。以下命令从本目录执行，所有 build、sdkconfig、Flash 和原始日志均置于仓外；`probe_work_dir` 与 `qemu_bin` 须换成实际绝对路径。

```bash
probe_work_dir=/absolute/private/probe-work
qemu_bin=/absolute/path/to/qemu-system-riscv32
source "$IDF_PATH/export.sh"
python3 ../../../tools/check_sdk.py --path "$IDF_PATH"
idf.py -C . -B "$probe_work_dir/build" -D SDKCONFIG="$probe_work_dir/sdkconfig" -D PROBE_STAGE=1 build
python3 run-qemu.py --build-dir "$probe_work_dir/build" --flash "$probe_work_dir/flash.bin" --stage 1 --qemu "$qemu_bin"
idf.py -C . -B "$probe_work_dir/build" -D SDKCONFIG="$probe_work_dir/sdkconfig" -D PROBE_STAGE=2 build
python3 run-qemu.py --build-dir "$probe_work_dir/build" --flash "$probe_work_dir/flash.bin" --stage 2 --qemu "$qemu_bin" --timeout-seconds 600
idf.py -C . -B "$probe_work_dir/build" -D SDKCONFIG="$probe_work_dir/sdkconfig" -D PROBE_STAGE=3 build
python3 run-qemu.py --build-dir "$probe_work_dir/build" --flash "$probe_work_dir/flash.bin" --stage 3 --qemu "$qemu_bin"
```

阶段 1 提交 revision 1–3 并验证旧 revision CAS 冲突；阶段 2 在新进程中读回 revision 3，再提交 revision 4–100；阶段 3 在另一新进程中读回 revision 100。runner 仅替换合成 Flash 的 factory app 区，原八页 NVS 保持连续。成功必须同时满足每步摘要、旁侧 blob、重启一致性和预期步数。`evidence/` 保留了本轮每代脱敏 SHA-256 与 NVS 统计；原始 QEMU 日志和完整合成 Flash 不入仓。

可从阶段 1 的仓外 Flash 副本再运行 `--stage 2 --stop-after-revision 37 --log-prefix cut37-stage2`，在第 37 代所有 SDK 提交、读回和 `nvs_get_stats` 返回且整行日志完成后结束 QEMU；随后阶段 3 使用 `--expect-revision 37 --log-prefix cut37-stage3` 回读。此操作只验证已返回写入后的进程终止，不模拟写入中的突然断电。

结果与边界见[八页 NVS 容量记录](../../../docs/operations/c3-eight-page-nvs-capacity.md)。禁止对设备执行 `idf.py flash`。
