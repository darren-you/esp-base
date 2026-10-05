# C3 MQTT 配置所有权消费检查点

2026-10-02，在 Base `89a29df27cd43a445a2daba2fcfd438734936dbf` 的代码上精确消费 MQTT `9d0495b3c8dda8ed13d691c1337beddf2976227c`，完成两个目标的普通构建、测试签名、官方验签与完整 host 回归，以及 C3 的联合功能复测。普通内部堆仍未达到五能力资源门，首版尚未完成。

## 消费边界与软件验证

唯一依赖变更为 MQTT manifest 的完整提交，以及官方 Component Manager 生成的 `dependencies.lock`、`dependencies.lock.esp32`。两锁只改变 MQTT 版本、组件摘要与 manifest 摘要；NVS 容量探针两锁、OTA／FRP／Container／WAMR 提交及其它输入保持。Base 的运行源码未修改，实验观察仅加每秒一次只读 heap／控制任务栈 trace。

初次 SDK 构建虽然完成，精确消费检查发现其 managed MQTT 和两锁仍指向旧 `50c9c45...`，因此拒绝作为新版本验证，失败输入与日志单独冻结。固定 Component Manager 3.1.2 的 `idf.py update-dependencies` 只构造默认锁路径的 manager，不能清除 Base 指定的 ESP32 锁。最终通过同一官方 `ComponentManager` API 显式指定每个目标的 `lock_path`，执行原生更新和 SDK reconfigure；锁文件内容与摘要均由 SDK 生成，没有修改 SDK、依赖缓存或手填锁内容。

编译前核对每份目标锁的实际 MQTT SHA，并核对 managed runtime 的 SHA-256 为 `bc11ed8f8f437b03c63c1d7a1753c8f7689882335c7af17e00bc0d2b68ac372c`。普通、签名及实验构建均消费该源码，原始非锁源码和 NVS 探针锁逐字节核对。

工具链固定为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、GCC 15.2.0。C3 签名 app 为 1183744 字节／槽 1245184 字节；ESP32 为 1114100／1179648 字节。两种签名均由官方工具验证，ESP32 普通构建明确使用 offline probe，未选作刷写候选。

MQTT 私有 runtime 从 8056 降至 2416 字节，另持有实际长度加一的 CA，最大 4097 字节，生命周期持续至 SDK destroy 成功。公开结构、订阅、队列、outbox、TLS 配置和其它限额保持；其软件所有权验证见 MQTT 源仓的检查点。该局部尺寸差不能直接换算为整机可用堆。

## C3 功能与恢复

使用正式布局、原始 app 入口及上述精确依赖；实验配置仅测试 CA、timer 授权／一个定时器、目标版本 `0.2.0-c3-lab-mqtt-memory-c` 和只读 trace，没有注入调用方或健康事实。

公开产品安装、一次 WRITE 联合 OTA、一次 MQTT restart、十二项消息计数业务及公开卸载均通过。来源下载 139 份采样全部保持 MQTT／FRP ready；目标及第三 boot 的 FRP 认证状态、确认产品、设备身份、revision 3 和原 OTA 成功结果核验。写命令均只提交一次。

完整写入回读、来源 A 保持及目标 C 精确签名镜像核对通过。结束时丢弃全部实验数据，恢复原 bootloader、partition table 和 factory app；三个代码区逐字节相等，原代码 Wi-Fi down ACK、实验服务停止与串口释放全部确认。没有 eFuse 写入；ESP32 已拔掉，本轮未触达。

## 实际资源结果

以下为每秒只读 trace，按真实 boot 分段。历史 heap 低水与采样 free／largest 分开记录；它没有覆盖每个瞬间，也未枚举全部任务。

| 阶段 | 最低历史 heap／B | 最低采样 free／B | 最低采样连续块／B | 控制任务最低栈余量／B |
| --- | ---: | ---: | ---: | ---: |
| 来源 CLI／产品安装 | 30040 | 57160 | 28672 | 2824 |
| 来源联合 OTA | 17980 | 29692 | 24576 | 2400 |
| 目标 C | 39040 | 56116 | 45056 | 2932 |
| MQTT restart 后第三 boot | 34740 | 51480 | 30720 | 2724 |

本轮控制任务采样余量超过 1024 字节，来源 OTA 的采样连续块达到 24576 字节；历史 heap 17980 字节仍低于 49152 字节，不能标记 P6-03 或完整容量通过。前轮历史 heap 为 11404 字节，两个独立样本的差额不全部归因于 MQTT 修改，也不据采样值声称连续块在所有瞬间达标。

满队列／outbox、FRP 双流／预备流／满长记录、完整 TLS／Wi-Fi 重连合法重叠峰值、全部任务栈、最大业务输入、百次整机生命周期、72 小时、掉电、ESP32 实板和生产入口仍开放。下一步依据实际对象及分配所有权继续收敛，保持 64 KiB guest、FRP 满长记录、TLS、签名、回退及原资源门。

## 冻结证据

本轮 308 份私有证据索引为 `2d7a483e2cc389d06d7b35d8b7a7eaef9647684d3d202d23636661967e49cfbd`，包含新版本 115 份 SDK 制品与日志、两目标锁、实验镜像、原始串口、写入回读、恢复及被拒绝旧依赖构建的证据。全部文件摘要核对。消费者源码归档为 `f4c19986e6a6bfe24280389be9b975ce700462594687bcee4aa379d01eb9bd9f`，SDK 制品归档为 `c91f2e1760fb54ff50824c659eb86b9a97353962917549354ee3f76d12ec3a3b`。

MQTT canonical 软件 71 份索引 `30b98062d38f197c1887fdc485eb1a1a7a318f091d4e409128506ef3cbfaafc8`、前序命令／控制栈与分配观察证据保持。设备身份、密码、密钥、签名私钥及恢复字节留在受控私有证据中，不进入公开仓。

命名批次后，Base `737b4e7c0a4d683983a81214842a8dbc0f0c46a4` 已采用 MQTT `f32335852d6f823c1a3b130bfbe3a7ac4499e10a` 及 SDK 生成的双目标锁。其实际运行、公开头文件和构建输入与本轮受测 `9d0495b...` 逐字节一致；本页原始版本与冻结索引保留，后续 RTC 所有权与当前依赖复核见[RTC 检查点](rtc-config-ownership-checkpoint.md)。

完整证据现保存于 ESP Tool 受限、Git 忽略的 `c3-validation-20261002/mqtt-owned-config-base-consumer`，原索引与全部文件摘要逐项核对，未改写拒绝旧依赖的失败收据。
