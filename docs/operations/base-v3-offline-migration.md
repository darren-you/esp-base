# ESP Base 旧 C3 配置离线预检

`tools/preflight_v3_migration.py`只读取旧C3 v1/v2完整Flash，核验后可生成EBCF v3单分区候选；它是[当前原生布局迁入工具](../../tools/prepare_native_layout.py)仍消费的一次性旧格式解析器，不是设备写入或当前整机迁入入口。当前双目标签名布局、旧终态、恢复与正式验收条件见[OTA操作](../../firmware/components/ota_operation/README.md)和[唯一执行计划](ota-allocation-diagnostic-checkpoint.md)。

旧C3布局由保留的[旧分区表](../../firmware/partitions/partition_table.csv)核对，`base_store@0x3e0000/0x20000`。预检要求两份不同普通文件、相同完整4MiB字节、当前用户拥有且0600或更严，以及本轮独立核对的UUID。官方NVS parser检查页、CRC、活动键、身份与配置；未知／重复记录、加密NVS、无效页、未决或非VALID选槽阻断。v1/v2字段转为v3时保留UUID、revision、已有凭据和有效字段，不生成替代密钥。

```bash
python3 tools/preflight_v3_migration.py \
  --backup-a <仓外第一份完整Flash备份> \
  --backup-b <仓外第二份完整Flash备份> \
  --idf-path "$IDF_PATH" \
  --device-id <本轮独立核对的设备UUID>
IDF_PATH=<固定SDK路径> python3 tools/test_preflight_v3_migration.py
```

可加`--output-base-store <仓外未存在的目标文件>`生成`0xb000`字节私有候选，以固定SDK官方生成器构造并逐键读回。候选只证明白名单活动记录和目标值，不证明旧页历史或全部Flash字节不变；脚本不打开串口、不刷写、不选槽、不改eFuse。输入拒绝及非阻塞普通文件检查的最新软件结果见[原生软件检查点](native_software_checkpoint.md#一次性迁入输入拒绝补审)。

## 旧恢复基线的限定结果

2026-09-26／27的P1-04私有旧C3恢复件中，第0页可读EBCF v1、112字节、revision5；后31页Invalid且非全空。完整预检因此拒绝，没有生成候选。四页与同备份旧应用槽整页相同，其余27页来源未定；[异常页只读分类](c3-base-store-page-forensics.md)保存原结果，不能把历史字节自行当作可擦空闲页。

同轮仓外[同键NVS探针](../../firmware/tests/nvs-same-key-probe/README.md)只证明固定输入／SDK的一次正常v1→v3提交改变第0页，尾31页字节保持；官方完整分区parser仍拒绝无效尾页。它不证明真实掉电、未来换页或长期保页，也不能放宽正式预检。旧基线与当前设备持久状态分别核对，本记录不构成新的设备接入或保留／丢弃授权。

当轮源码、私有Flash和恢复材料留在既有受控位置。本任务仅删除退役运行依赖与实验入口，旧解析器继续服务一次性保留数据迁入；真实写入仍须按当轮设备和完整恢复方案单独验收。
