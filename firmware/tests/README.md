# 固件测试

公开入口 `bash firmware/tests/run_host_tests.sh` 使用官方Component Manager解析的同target组件，开启ASan／UBSan和严格编译诊断。ESP_BASE_TEST_TARGET选择esp32c3或esp32；ESP_BASE_TEST_COMPONENTS_DIR可指向本轮独立源码副本的已解析managed_components，不从相邻源仓import。

运行前先按[固件构建说明](../README.md)准备锁定SDK，并对所选target执行官方Component Manager重新解析。`managed_components`是生成目录，新检出或清理生成物后须先恢复该输入；host入口本身不下载依赖。使用已冻结的独立构建输入时，分别指定各target对应的完整组件目录，不能把空目录、另一轮缓存或相邻源仓当成本轮组件：

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

原生业务确定性测试覆盖既有十二项语义、二进制零字节、最大输入、暂停计数保持、100ms非延期窗口、恢复和重新初始化；不模拟MCU执行时延。命令解码测试覆盖新固件／业务／FRP字段、旧product.*拒绝、USB与FRP来源隔离、重复／未知字段、UTF-8、数值、分片与10000次畸形输入。分配测试核对失败不写、秘密清零、所有权与释放。

真实protocol owner测试编译生产decoder／guard／control／业务／协议源码，用显式SDK、NVS与OTA假件覆盖写前意图、原请求移交、双入口互斥、FRP arm／5秒无连接、任务失败、成功prepare后取消不选槽、部分清C、unknown锁保留、原ID回放和独立firmware.status。receipt／firmware测试核对182B V4、V3／损坏阻断、A/C完整签名身份、pending／VALID／失败与存储调用故障。startup两目标分别启用／禁用scratch，验证本地30秒控制进展、跨窗、rollback、确认读回不确定与短Flash仲裁。均不冒充真实NVS掉电原子性或bootloader。

`flash_io_concurrency_test.c` 直接编译生产 main 的短 Flash 回调与真实 storage owner：两个宿主线程交接、8192次竞争 I/O 和原500ms BUSY期限，等待者只访问自己的局部 claim，释放前清理共享交接。公开 host 入口包含ASan／UBSan；独立TSan结果与曾复现的数据竞争另存当轮检查点。测试不模拟MCU调度、Flash最坏时延或实时栈水位。

MQTT owner／认证测试保持QoS1、non-retained、严格TLS、双SUBACK、4096入站／5120发布／16384outbox、原生事件boot／连续序号及HMAC、重配／重连／失败释放。Wi-Fi／时间测试验证真实组件的初始化失败与恢复，但不模拟无线AP、DNS或NTP。

```bash
ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_frp_management_crypto_tests.sh
ESP_BASE_TEST_TARGET=esp32 bash firmware/tests/run_frp_management_crypto_tests.sh
```

独立crypto入口需OpenSSL与pkg-config，真实loopback TCP、HMAC、decoder和Base handler核对原status／restart固定向量、签名错误、响应篡改、原ID回放与延迟一次重启。普通listener矩阵另覆盖六新命令、绑定PUT上传、分片／预读、错HMAC、framing、截断／多余字节、并发查询、真实读线程与configure shutdown／fd回收。

通用HTTPS／有界输入流、完整镜像／签名／Flash／slot机制回归归属于精确esp-ota组件，不保留第二份实现。旧Container生命周期、包source／账本、guest与专属探针测试已退役；历史证据继续保存，但没有现役编译入口。

宿主Python控制、FRP客户端、事件发布、官方分区和离线迁入见[tools](../../tools/README.md)。官方 FRPS 的 Python 场景消费者也位于 `tools/frps_ota_scenario.py`；本目录保留 C／Go 夹具与 Shell 入口。本轮输入／实际结果见[原生软件检查点](../../docs/operations/native_software_checkpoint.md)，双板真实负载、断电、百次与72小时仍待[执行计划](../../docs/operations/ota-allocation-diagnostic-checkpoint.md)取得独立证据。
