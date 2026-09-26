# ESP32-C3 八页 NVS 合成容量验证

2026-09-26，在 Base 独立工作树中，使用[合成容量探针](../../firmware/tests/nvs-capacity-probe/README.md)验证连续八个 4 KiB 页能否承载最大 Base v3 配置和两份旁侧记录。固定 ESP-IDF 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，实际 lwIP checkout 为 `2758df4cd3666b3b2a5b53830148379326425c0d`，QEMU 为 Espressif `9.2.2 (esp_develop_9.2.2_20260417)`。SDK fork 当前 gitlink 指向另一 lwIP 提交，构建会发出 submodule out of date 提示；本探针明确按 Base `sdk-lock.json` 固定的 checkout 构建。生成 `sdkconfig` 核对为 `esp32c3`、4 MiB Flash、自定义合成分区表、NVS 加密关闭。

本次运行的公开输入收据：`main/nvs_capacity_probe.c` SHA-256 `a63eb4c10ec38f0cff0d4d1b7e979e4fbab76c9dda587d9c770628ed458f2621`；`run-qemu.py` SHA-256 `dd05de09517f8a6aa2a259471250e0ee1ded51b150f24a52d0cd3aaf1e3d78c6`；合成 `partitions.csv` SHA-256 `106a690443b1a99ed5aec04eefd7875020a0bebdd62200a3f9c2dc133bc8f1dc`。远端执行副本与仓内源码摘要一致。三阶段构建与 QEMU 命令见探针 README；官方解析命令为 `nvs_tool.py -i -d none <仓外提取的合成八页 NVS>`。

## 输入与结果

- 仅用人工构造的有效 Base v3 配置。每代规范编码长度均为上限 **7,618 字节**，管理 key 与 revision 改变；阶段 1、2 合计 revision 1–100 的 100 个读回 SHA-256 全部不同。
- 同一 `base_store@0x138000/0x8000` 内，真实 `base_config/committed` CAS 每代成功；真实 `base_ota/operation` 键每代写入并读回 118 字节的合成收据形态值；合成 `base_container/state` 键每代写入并读回 288 字节占用值。Container 产品键尚未冻结，未验证 metadata 解析；OTA 未运行注册策略或下载。
- 阶段 1 的三代 CAS 成功，旧 revision 请求返回预期冲突码。阶段 2 是新 QEMU 进程，在任何新写入前读回 revision 3 和两份旁侧 blob，随后完成 revision 4–100。阶段 3 又用同一合成 Flash 的新进程读回 revision 100、配置摘要和两份旁侧 blob，`PROBE_RESTART_MATCH=1`。没有 NVS API 失败或读回不一致。

| revision | used | free | available | total | namespaces |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 262 | 746 | 620 | 1008 | 3 |
| 3 | 263 | 745 | 619 | 1008 | 3 |
| 10 | 263 | 745 | 619 | 1008 | 3 |
| 50 | 263 | 745 | 619 | 1008 | 3 |
| 100 | 262 | 746 | 620 | 1008 | 3 |

`nvs_get_stats` 的 `available` 在 revision 15、29、43、57、71、85、99 从 619 回升 620。更直接的页证据：阶段 1 后页序号从 0 开始，页状态为六个 Full、一个 Active、一个 Empty；第 100 代后有一个 Active、一个 Empty、六个 Full，最高页序号达到 **207**。固定 SDK 官方 `nvs_tool.py -i -d none` 对最终八页逐页报告 CRC32 OK 或 Empty，返回码 0。统计值周期回升与页序号、状态共同证明 QEMU 中发生换页及回收；不把 `available` 的单独波动当作 Flash 擦除证明。

另从阶段 1 的同一合成 Flash 副本重跑阶段 2，在 revision 37 的配置 CAS、两份 blob 提交与读回、`nvs_get_stats` 返回且完整日志行输出后，强制结束 QEMU。再启动阶段 3，读回 revision 37 的三份值且相互匹配。此为**已返回写入后的进程终止**，不代表写入中断电、真实 Flash 的磨损或供电故障恢复。

逐代摘要与统计、阶段重启和受控终止证据保存在[合成日志目录](../../firmware/tests/nvs-capacity-probe/evidence/README.md)。日志不含配置原文、私有备份、设备 UUID 或真实凭据；完整合成 Flash、原始 QEMU 串口输出与构建产物留在仓外。

## 边界与后续

本轮证明固定 SDK 在 QEMU 正常路径下，八页可完成 100 代最大 Base 配置 CAS 与两份指定长度 blob 的共同写入、回收和重启读取。没有测试七页或更小容量，因此不声称八页是数学最小值。合成 factory app 与分区表只用于容量实验，**产品 CSV 未修改**；尚未核对新布局对双槽应用、bootloader、签名镜像、旧分区保留和 OTA 回滚的全链路约束。Container 仍需在真实产品键、metadata 编码及调用入口冻结后重跑。实板写入及写入中断电未授权也未验证，现有完整旧 `base_store` 的正式 preflight 仍阻断。
