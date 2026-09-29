# C3 产品布局十一页 NVS 容量验证

2026-09-29，以 `esp-base@f66619dc15f1dbb4717507f627ce323a043e48e2`、固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、锁定 Container 组件和 Espressif QEMU `esp_develop_9.2.2_20260417`，运行 `firmware/tests/nvs-capacity-probe` 的 11 页变体。变体的数据区与正式 C3 产品源码一致：三份 `0x77000` 包槽、`frp_scratch@0x3e5000/0x10000`、`base_store@0x3f5000/0xb000`。为从空白合成 Flash 启动探针，仅将 `ota_0@0x20000/0x130000` 替为同区域 factory app；不把该测试分区表用于产品固件或实体板。

每代在同一 `base_store` 分区提交并读回最大 7,618 字节 v3 配置、186 字节 OTA V2 收据形态、正式 Container ECS2 编码的 288 字节绑定，以及 910 字节最近八条产品操作账本。产品账本每代提交 `PREPARED` 和 `SUCCEEDED` 两次。阶段一从空白 Flash 完成 revision 1–3 和旧 revision CAS 拒绝；阶段二使用同一 Flash 的新 QEMU 进程读回 3 并完成 4–100；阶段三再次冷启动读回 revision 100。三阶段返回码均为 0，100 条 `PROBE_STEP` 的四类记录校验均为 `ok`。

最终启动输出 `PROBE_RESTART_MATCH=1 container_decoded=1`，ECS2 sequence 为 101，配置／OTA／产品记录 revision 均为 100。11 页 NVS 最终 `total=1386`、`used=298`、`free=1088`、`available=962`、`namespaces=4`；固定 SDK 官方 NVS parser 对非空页均报告 `CRC32: OK`。仓外最终 4 MiB 合成 Flash SHA-256 为 `f56f8d3285fbebb4dedaebc330bc4ef9014ed364ad0d200a7c07924d8c41706a`，提取的 `0xb000` 字节 NVS SHA-256 为 `91afd0824d01dfa2f2f68c1799a15471fb393cfe55e321e7823dcb55f0d621e8`，测试分区表 SHA-256 为 `fea8e3f611862312d359a6460a4ea72d87545d95a6e225146c8786fe44ef6c37`。

本探针只覆盖当前四种记录形态、合成数据和 QEMU 的重复提交／冷启动；它没有运行新布局下的正式 Base、真实包、Broker、FRP 或 OTA 下载，也没有测量实体 Flash 擦写寿命和断电结果。未来联合 OTA 收据结构变化后须按实际新长度重跑容量验证；P6-03／P7-01 不因此验收。
