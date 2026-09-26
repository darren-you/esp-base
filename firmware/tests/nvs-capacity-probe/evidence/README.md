# 合成 QEMU 证据

本目录只保留 `PROBE_` 行及固定 SDK 官方 NVS parser 的无内容完整性输出。所有配置与收据均为合成值；没有私有设备 Flash、身份、凭据或完整镜像摘要。

| 文件 | 范围 |
| --- | --- |
| `stage1-evidence.log` | revision 1–3；当前源码的独立复跑与首次输入逐字节相同，旧 CAS 冲突码已断言 |
| `stage2-evidence.log` | 新进程先读回 revision 3，继而完成 revision 4–100，每代配置摘要及 `nvs_get_stats` |
| `stage3-evidence.log` | 再次新进程读回 revision 100 与两份旁侧 blob |
| `cut37-stage2-evidence.log` | 从阶段 1 的合成 Flash 副本出发，revision 37 完整返回后终止 QEMU |
| `cut37-stage3-evidence.log` | 终止后的新进程读回 revision 37 与两份旁侧 blob |
| `parser-integrity.log` | 固定 SDK `nvs_tool.py -i -d none` 检查最终八页：七页 CRC32 OK、一页 Empty |

前后页状态与容量结论见[八页容量记录](../../../../docs/operations/c3-eight-page-nvs-capacity.md)。`stage1`、`stage2`、`stage3` 是同一合成 Flash 的连续阶段；`cut37` 使用阶段 1 后的独立副本。
