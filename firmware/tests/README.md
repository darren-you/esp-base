# 固件测试

MQTT OTA 二进制编解码和单块状态机的独立入口为 `bash firmware/tests/run_mqtt_ota_tests.sh`，需要 OpenSSL 与 pkg-config；它直接编译正式 C 源码及 PSA→OpenSSL 测试适配，开启 ASan／UBSan。独立 Python HMAC 固定向量与 Server Go 测试逐字节一致，覆盖全帧逐字节篡改／每个截断长度、错误身份／方向／topic／QoS／retained，以及有效 MAC 下的坏域和字段。状态机覆盖借用数据复制、实际写确认前背压、重复／冲突、尾块 END、取消、无进展期限和时钟倒退。两个真实宿主线程同时运行控制端和 worker，使用 pthread 临界区与 SDK／Flash 假件交付 40 个完整块及尾块，再核对 EOF、延迟写确认和关闭重用；`ESP_BASE_TEST_THREAD_SANITIZER=1` 另执行这段并发的 TSan 构建。这些测试不访问 Broker、NVS 或实体设备。状态机另覆盖验签期间取消／超时／冲突、尾块未写不能选槽，以及选槽开始后的迟到取消。

公开入口 `bash firmware/tests/run_host_tests.sh` 使用官方Component Manager解析的同target组件，开启ASan／UBSan和严格编译诊断。ESP_BASE_TEST_TARGET选择esp32c3或esp32；ESP_BASE_TEST_COMPONENTS_DIR可指向本轮独立源码副本的已解析managed_components，不从相邻源仓import。

运行前先按[固件构建说明](../README.md)准备锁定SDK，并对所选target执行官方Component Manager重新解析。`managed_components`是生成目录，新检出或清理生成物后须先恢复该输入；host入口本身不下载依赖。使用已冻结的独立构建输入时，分别指定各target对应的组件目录，不能把空目录、另一轮缓存或相邻源仓当成本轮组件。也可提取该入口实际读取的源码／头文件子集，但必须逐文件核对长度与摘要、绑定原target锁和解析来源；子集通过只证明host消费，不等于完整SDK解析或固件构建：

```bash
ESP_BASE_TEST_TARGET=esp32c3 \
  ESP_BASE_TEST_COMPONENTS_DIR=/absolute/private/esp32c3/managed_components \
  bash firmware/tests/run_host_tests.sh
ESP_BASE_TEST_TARGET=esp32 \
  ESP_BASE_TEST_COMPONENTS_DIR=/absolute/private/esp32/managed_components \
  bash firmware/tests/run_host_tests.sh
```

下列简写适用于仓内默认`firmware/managed_components`已按锁定合同解析的情况；对应软件收据须绑定实际消费源与target。

```bash
ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_host_tests.sh
ESP_BASE_TEST_TARGET=esp32 bash firmware/tests/run_host_tests.sh
```

原生业务确定性测试覆盖既有十二项语义、二进制零字节、最大输入、暂停计数保持、100ms非延期窗口、恢复和重新初始化；不模拟MCU执行时延。命令解码测试覆盖固件／业务字段、已退役命令拒绝、USB URL 与 MQTT 元数据来源隔离、schema 4 配置、重复／未知字段、UTF-8、数值、分片与10000次畸形输入。分配测试核对失败不写、秘密清零、所有权与释放。

`producer_capacity_test.c` 直接编译正式诊断 producer，覆盖晚出生 region 的双下界为零、caps／alias 不重复、独立 IRAM 域、BEGIN 先于 getter、任务名在 scheduler resume 后仍安全、创建／最终化／存活计数闭合、低于 1024 B 的真实输出和帧编号溢出。SDK 假件只检验输出与门，不授 MCU 成本或 R5／R6。实际 SDK 分配器／最终任务栈及派生拒绝用例另通过 tools 的 `test_sdk_capacity.py`／`test_managed_sdk.py` 验证。

真实 protocol owner 测试编译生产 decoder／guard／control／业务／协议源码，用显式 SDK、NVS 与 OTA 假件覆盖写前意图、原请求移交、USB／MQTT 双入口互斥、任务失败、成功 prepare 后取消不选槽、部分清 C、unknown 锁保留、原 ID 回放和独立 firmware.status。receipt／firmware 测试核对登记只消费一次真实观察入口、现有事务 claim、槽状态／几何变化与 claim 失效停止、坏签名及未决前次阻断、前次失败来源证明在同一登记事务内复用且不增加完整验签、来源摘要／物理槽不符仍保留旧收据，182 B V4、V3／损坏阻断、A/C 完整签名身份、pending／VALID／失败与存储调用故障。观察测试分别编译真实 firmware observer 与 receipt register，SDK 验签／NVS 假件验证控制边界；不冒充实体密码学或公网 ACK 时延。startup 两目标验证本地 30 秒控制进展、跨窗、rollback、确认读回不确定与短 Flash 仲裁，不初始化闲置 scratch。均不冒充真实 NVS 掉电原子性或 bootloader。

`config_store_test.c` 覆盖旧 v3 扩展配置的严格校验、同 key 一次性规范化写入和 SHA-256 读回核对；保持 v3 持久格式、原 revision 与 Wi-Fi／MQTT 字节。用最大 7618 B 旧记录、全部截断、损坏字段、写入前后故障、读回不一致和第二次启动验证失败关闭与不重复写入。OpenSSL 提供真实 SHA-256，NVS 仍为故障假件；通过不证明真实掉电原子性。

同一 owner 回归还覆盖串口初始化：ESP32 RX 驱动分配失败时不切换 VFS，非阻塞设置失败时退回原 VFS 并释放本次驱动；C3 保持原读取标志。假件验证错误传播和清理，不模拟硬件 FIFO、中断调度或 Flash 停用 cache；合法最大串口输入与周期输出重叠仍须在实板验证。

`flash_io_concurrency_test.c` 直接编译生产 main 的短 Flash 回调与真实 storage owner：两个宿主线程交接、8192次竞争 I/O 和原500ms BUSY期限，等待者只访问自己的局部 claim，释放前清理共享交接。公开 host 入口包含ASan／UBSan；独立TSan结果与曾复现的数据竞争另存当轮检查点。测试不模拟MCU调度、Flash最坏时延或实时栈水位。

Flash 回调测试同时核对 held 起点随 claim 交接、释放失败成对恢复、倒退时钟不使 BUSY 无限等待，以及成功取得 claim 后仍走真实释放。`flash_observation_test.c` 直接编译 Base 自有观测原语，覆盖 OTA／配置两类并发快照、32位计数与时长饱和、负／倒退时钟永久失效、失败释放不混入完成 claim 最大值和独立 UART 前缀。协议 owner 测试核对配置单次 BUSY 不延后原 Wi-Fi proof deadline，下一次真实成功才增加完成计数；这些均不授实体联合延迟或 R5。

MQTT owner 的独立入口为 `bash firmware/tests/run_mqtt_owner_tests.sh`，使用锁定的 mqtt 头文件／合同源码及 OpenSSL 真实 HMAC，保持 QoS1、non-retained、严格 TLS、三订阅全部 SUBACK、4096 入站／5120 发布／16384 outbox、原生事件 boot／连续序号、重配／重连／失败释放。新增 OTA operation 绑定、错误 boot／operation／key／topic 拒绝、最多一个未 PUBACK 块请求、应用请求重试限速、断线保留绑定与重配撤销。协议 owner 回归分别验证 USB URL 与 MQTT 元数据 start、同 owner 互斥、重读意图后才请求数据、实际写回调、验签后取消禁止选槽，以及失败收据和 unknown 的存储门禁。Wi-Fi／时间测试验证真实组件的初始化失败与恢复，但不模拟无线AP、DNS或NTP。

通用HTTPS／有界输入流、完整镜像／签名／Flash／slot机制回归归属于精确esp-ota组件，不保留第二份实现。当前测试图只覆盖保留的原生固件、网络、OTA与恢复；退役动态运行测试及专属探针已删除。

宿主 Python 控制、事件发布、官方分区和离线迁入见[tools](../../tools/README.md)。历史输入／实际结果见[原生软件检查点](../../docs/operations/native_software_checkpoint.md)，双板真实负载、断电、百次与72小时仍待[执行计划](../../docs/operations/ota-allocation-diagnostic-checkpoint.md)取得独立证据。

SDK 假件职责目录统一为 `fakes/app-main`、`fakes/network-auth`、`fakes/mqtt-ota`、`fakes/ota-firmware`、`fakes/ota-update` 和 `fakes/wifi-runtime`。读取入口是本目录的 `run_host_tests.sh`、`run_mqtt_owner_tests.sh` 与 `run_mqtt_ota_tests.sh`；它们不属于 ESP-IDF 组件，内部 SDK 头文件及 `psa/crypto.h` 等入口保持原合同。
