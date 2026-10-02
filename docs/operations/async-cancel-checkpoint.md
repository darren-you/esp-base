# 产品 owner 异步取消检查点

2026-10-02，P6-07／P6-09 继续实施；宿主软件、固定 SDK 两目标签名构建、C3 直接 API 调度对照及正式 Base 签名产品的原生 event／timer 停止切片已验证。公开管理入口、联网组合与 ESP32 实板仍待验收。

## 精确输入与源码归属

- Base 改动基于 `1de99373da89e4c13a195cd3fb55d8d742e55f06`。
- Container 固定公开提交 `2b93b979b8b0760dcb96b28ac5d13fc52ae547bf`；WAMR 固定维护 fork 提交 `74fd95ccbdc417c3816e04f3308eea8a5473ed34`，此前为 `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。
- MQTT 固定公开提交 `50c9c45f0fe95d4e99ab39584ff04d45d432efbc`；其相对 `cc5be035aaed6476a28c4704b788e985933be7a5` 仅修改组件打包合同、宿主测试和文档，C／H 运行源码不变。真实 Component Manager 3.1.2 的两个独立缓存取得相同摘要 `9a5d2846eecb57456e745aa6f582cf014b96ec7603ee30a4261ce59bff35f616`、433 个逐字节相同文件，归档不含子模块 `.git` 定位文件；见 [MQTT 记录](https://github.com/esp-space/esp-mqtt/blob/50c9c45f0fe95d4e99ab39584ff04d45d432efbc/docs/verification/component_hash_reproducibility.md)。
- 主固件与 NVS 容量探针的四份目标锁由官方 Component Manager 强制重新解析，并在完整固定 SDK 的实际 CMake 入口重新生成；Container／WAMR／MQTT 之外的依赖版本、摘要保持不变。SDK 仍固定 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP 固定 `2758df4cd3666b3b2a5b53830148379326425c0d`。
- 宿主编译使用真实 wasi-sdk 33.0；不以改写 SDK 的 VERSION 或组件摘要来通过校验。

## 真实缺口与执行合同

此前的 Classic 配置关闭 guest 线程管理。另一 native 线程调用 `wasm_runtime_terminate` 不能中断纯 Wasm 循环，500 ms 探针最终由既有期限退出，取消后仍执行约 359 ms。维护 fork 新增由执行 owner 设置、读取与清除的取消谓词，沿 Classic 安全检查点及 native 导入前后轮询；其他 native 线程只写其自有原子标志，不改 VM 异常或期限，也不启用 guest 线程管理。

Container 将入口取消返回为 `ENTRY_CANCELLED`，撤销本入口日志与计时器并禁止继续业务调用；取消后的真实 `stop` 使用独立预算和期限，取消标志不阻止清理入口。Base 将既有 `stop_requested` 绑定到平台限额，取消 init／event／timer 后由唯一 guest 线程执行真实 stop、close，调用方取得 stopped 信号并 join 后才承认停止。停止失败或超期仍阻断重开；不会把中断的业务事件计为成功或试运行失败，不额外改 ECS2／包字节。

本变更不裁决公开手动停止的跨重启语义；启动过程 init 被取消后，现有同 boot 重开门仍保持关闭。同步阻塞 native／OS 调度没有硬抢占保证。

## 宿主验证

精确、干净的公开 Container／WAMR checkout 与 wasi-sdk 路径通过以下入口运行；`TEST_PYTHON` 同时提供给 CMake guest 构包和直接 Python 测试，解释器需安装 `cryptography`。

```bash
TEST_PYTHON=/absolute/python ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_container_lifecycle_test.sh /absolute/container /absolute/wamr /absolute/wasi-sdk
TEST_PYTHON=/absolute/python ESP_BASE_TEST_TARGET=esp32 bash firmware/tests/run_container_lifecycle_test.sh /absolute/container /absolute/wamr /absolute/wasi-sdk
ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_host_tests.sh
ESP_BASE_TEST_TARGET=esp32 bash firmware/tests/run_host_tests.sh
```

两目标完整 host 与真实 RSA 签名 guest 生命周期均通过，Base 测试二进制启用 ASan／UBSan。新增五个场景分别为纯 Wasm init 取消、event 取消、timer 取消、event 取消后 stop 返回失败，以及 event 取消后 stop 纯循环超期。init 用例由另一 native 线程直接置既有原子标志，不证明公开管理面存在启动中取消入口；其余场景调用正式内部停止 API。成功 event／timer 停止以 250 ms 宿主观察门与原 500 ms 策略期限区分取消和等待期限，随后实际重开并再次停止。失败场景保持 BLOCKED、禁止重开，绑定、NVS 与包 Flash 写计数不变。

原有签名包安装、启动、停止、卸载、同 boot EMPTY 和重装仍各运行 100 轮，ECS2 1→601。普通 macOS 分配统计第 10／50／100 轮 malloc 均为 384864 B；C3 分支虚拟字节均为 445752901632、region 均为 71，ESP32 分支虚拟字节均为 445752590336、region 为 70／70／72。region 拆分与 malloc／虚拟字节增长分别记录，不能宣称所有指标完全持平。此统计不是 RSS、ESP RAM 或设备回收证明。

真实 Container 的 goto／switch 分派全套 CTest 各 9／9 ASan／UBSan 通过，新增各 100 次取消／close／重开；其普通分配统计 malloc 均为 11104 B、虚拟字节均为 445751787520、region 均为 62，见 [Container 检查点](https://github.com/esp-space/esp-container/blob/2b93b979b8b0760dcb96b28ac5d13fc52ae547bf/docs/operations/async_cancel_checkpoint.md)。

## 固定 SDK 签名构建

首次独立归档输入为 Base `8990128674a8f680ca1813474f2268ddda393b84`，归档 SHA-256 为 `894a59135ee5e61d94c1f7aa42d3d4b0630cab0158b2ca5df61411bc748735a2`。完整固定 SDK 校验、两目标官方构建、签名验证、槽容量门及宿主工具 47／47 通过；NVS 容量项目以 C3 十一页／ESP32 六页分别完成实际 CMake 解析。生成锁相对上述输入只改变主固件两锁的 `manifest_hash`，所有依赖字段、target 和其余字段逐项一致；本仓采用 SDK 原样输出，不手改摘要。下表记录调度修正前的镜像，修正后的完整重编译见下节。

| target | 测试签名 | app 字节／槽字节 | app SHA-256 |
| --- | --- | --- | --- |
| esp32c3 | RSA v2 | 1183744／1245184 | `9df8506b0660620739c372544fcdf352ff314b23252a2532e7ad9f8e0b580807` |
| esp32 | ECDSA v1 | 1114100／1179648 | `b3746150d51e8277b4be95ae41390addb0e6567ead05c2c0706592d25a1930c9` |

使用仓外临时测试键、无网络凭据的 counter 产品授权、100000 指令、100 ms 期限、16384 B owner 栈；不启用硬件 Secure Boot，也不写设备。两个 ELF 均包含实际 Base 启动入口与取消异常；ESP32 另保留 WAMR 取消设置及 Container open／init 符号，C3 原有选择性 LTO 内联这些符号，不能要求它们以独立符号存在。这只证明当前组件编译和链接，不证明目标调度中的取消响应或生产发布。

## FreeRTOS 请求者调度复现

固定 SDK、同一 WAMR 取消实现的独立 C3／ESP32 单核 QEMU 探针复现默认 pthread 优先级 5 高于 Base 产品 worker 4 的问题：纯 Wasm 循环分别在 500099／500250 微秒后由期限结束，请求者再过 37083／17285 微秒才写入标志。最终对照在执行者入口实际调用 `vTaskPrioritySet(NULL, 3)`，保持全局 pthread 默认优先级 5；两目标由取消结束，请求到退出分别为 31／52 微秒。两份日志均取得完整成功标记后结束本次 QEMU 进程。这证明固定 SDK 调度与 WAMR 检查点的关系，尚不证明正式 Base 产品管理面或实体板响应。

最终独立探针源码 SHA-256 为 `dbf24a1417e0521d39a88679337fba32b07afa7d506f1fcef09ecb9d7c880035`；C3／ESP32 app 摘要分别为 `acd652ab1ec2c9f2d7457c6cc9c4d67f8bc5539ec6a81b76697267ed454f41b7`、`d50c8c730b5a12760e29ed7689621c7b572442210ecfc1dce0746237f06c4817`，完整 UART 日志摘要分别为 `c24b6a249ffa92e1c80a1cb6644880c4cb6698ff27bda1e742bf17f154c18e7a`、`6dbb69bd02b161299b42b8ff76323e15a811021e92b0196a7ece0b6535b0fdb8`。

Base 现仅在唯一 guest 线程入口调用 SDK 的 `vTaskPrioritySet(NULL, 3)`；产品 worker 4 与 control 5 保持既有设置。没有改全局 pthread 默认配置、其他 SDK 线程或取消回调的非阻塞合同。该新源码的双目标完整 host、五个真实签名取消场景与各 100 次生命周期已重新通过。第 10／50／100 轮 malloc 仍均为 384864 B；C3 虚拟字节均为 445752967168、region 均为 69，ESP32 虚拟字节均为 445752311808、region 均为 70。

修正源码 `9564d74f7bd7dfa64334680e1c07a8eca2abc9ef` 归档 SHA-256 为 `c70f5482e6f855070b9fdd049b64304c698f3c0bb4de2edef21b107a266134ff`。同一完整 SDK 的双目标签名构建、官方验签与容量门再次通过，四份锁逐字节保持不变，两个 ELF 均链接真实 Base 启动入口、优先级 API 和取消异常。测试键与 counter 授权仍为上述仓外输入，不含网络凭据，不写设备。

| target | 测试签名 | app 字节／槽字节 | 调度修正后 app SHA-256 |
| --- | --- | --- | --- |
| esp32c3 | RSA v2 | 1183744／1245184 | `77d15d4bbb510663c1bdac88f6ccf581898c8b6101289aa5628d03a23bfec1bf` |
| esp32 | ECDSA v1 | 1114100／1179648 | `c78483debdb4701554346e0a05afa3f1f088fe48cb562df5c6a7ca0e6f226889` |

两目标 SDK 配置摘要分别为 `04c4a18c58308c77334a5c017e6f17d52aa7c1d38b21925272e5aeffb28371ea`、`b0caf6f17f2aed9f4862f158c506c7d1b6f2764a3839bd8292204c0b570f370b`。这次完整 Base 构建与独立调度探针分别记录，不把独立 QEMU 结果等同于签名产品全链验收。

## C3 实板调度切片

同一独立探针源码在固定 SDK 中仅将控制台改为 C3 原生 USB Serial/JTAG，得到 189440 B 的未签名实验 app，SHA-256 为 `827afa5bb7138cbe14689f7e4975fcbe7eeb678bdf14cce5e05ec1afbdfc78e6`，SDK 配置摘要为 `839cddc7ea84a0a2e8c489a03855a30b86b6f2bab5bb797b020202fcf45a6e42`。本轮核对唯一接线 C3 的 USB 身份、4 MiB Flash、关闭的 Secure Boot／Flash Encryption，以及此前实验的 bootloader、分区表和完整 factory 槽字节后，只暂时替换 `0x10000/0x100000` app 槽，写后完整读回。

实际 default owner 5 的循环在 500120 微秒后由期限结束，请求者又过 17809 微秒才写标志；owner 在入口自设优先级 3 后，循环由取消结束，请求到退出为 70 微秒，完整成功标记已取得。随后重新进入 ROM 下载模式恢复原 MQTT 实验 app；初次 `no_reset` 恢复失败的日志保留，受控复位后的完整槽读回与写前逐字节相同，bootloader／分区表读回未变。原 MQTT 应用确认 `wifi_down`，串口释放，没有 eFuse 写入。

这是实际 C3 调度与 WAMR 直接 API 的切片；未运行 Base 的签名包、唯一产品 owner 或公开管理命令，也未运行联网组合负载，不能将其视为 P6-07／P6-09 完整验收。ESP32 未连接，本轮没有对应实板结果。

## C3 签名产品原生停止切片

独立输入归档为 Base `d6cf8d32ab0cc77ae736695fce2f0ec6fe8052f6`，207 个固件文件的字节与执行位均等于公开 `61b4adc3bc7ce05424972f69cfd8e945fce50fd8`。本轮仅在仓外 `app_main` 注入优先级 4 的原生测试调用方；生产 Container、WAMR、Base 产品 owner、Flash／NVS provider 与正式 C3 分区几何保持原样。调用方源码摘要为 `c2112ee8d7d2a9dd759f4bbd45431d596c0d5dfc2a8b18eec21e3d417fe2254b`。

使用真实 wasi-sdk 33.0 编译本仓取消 guest，并由临时 RSA-3072 测试键签包。整包为 10240 B，SHA-256 `b559045e15356f95b25f52f1ce3f3d5b222b76df474926dd7fefb5a3d86bea48`；Wasm 摘要 `85cad4903d740887b45a9c6178fa4f330f94264fd79adad5284b6a1dc0071269`。timer 能力仅授予一份计时器，入口使用 500 ms 期限和 100000000 指令预算。正式 Container slot codec 离线构造 sequence 6 的确认绑定，Base ledger codec 构造空操作账本，SDK NVS generator 原样生成 11 页 `base_store`；这两份状态是测试输入，不是设备公开安装或成功操作记录。

固定 SDK 的 1183744 B app 经官方 RSA v2 验签，摘要为 `d709539022bc36d9c0f1eaf03e525c4278b889fb9ba94679e34a800b50ccbad5`；4 MiB 完整实验 Flash 摘要为 `caef9da5eaff24ce9872c5d92ee0629991796e216bd920a1653d379dfb16053b`。核对唯一 C3 的真实身份、Flash 与安全状态后，按维护者丢弃 ESP 数据的授权执行整片擦除、写入和完整读回。实际 Base 达到 READY／RUNNING，guest 线程优先级为 3；调用方跨两个真实 tick 观察纯 Wasm 循环仍可运行，再调用正式内部 `stop_confirmed`，等待真实 stop、close、stopped 信号与 join。

| 场景 | stop／close／join 微秒 | 成功事件计数 | 回收后可用堆 B | 历史最低堆 B | 回收后最大块 B |
| --- | --- | --- | --- | --- | --- |
| event 长循环取消 | 690 | 0 | 201316 | 98776 | 114688 |
| 同 boot 实际重开后的 timer 长循环取消 | 851 | 1 | 201308 | 98776 | 114688 |

timer 的 1 是启动计时器事件已完成，取消的计时器回调没有增加成功计数。完整本轮 UART 日志摘要为 `b3b6ddc3ab6e638b2f96a5265e01011c3fda978006574c9b46dafe51bbabb340`。本轮结束后再清空实验数据并恢复原 bootloader、分区表与完整 factory 槽，三份代码均逐字节读回相同；原 MQTT 应用确认 Wi-Fi 关闭，串口已释放，没有 eFuse 写入。旧身份、配置和实验 NVS 均已丢弃，不宣称恢复旧数据。

该切片运行真实签名 Base 产品与 MCU provider，但调用方在本地固件内；没有网络负载，也不证明公开 MQTT／USB 停止、init 的公开取消、设备安装／卸载、跨重启停止策略或 ESP32 实板。失败轮单独保留，未补记为通过。

## C3 原生停止百轮回收

同一生产输入与正式分区的另一个仓外调用方交替执行 100 轮 event／timer，实际完成 200 次 stop／close／join 和 199 次重开。调用方仅对未接受的 `EVENT_BUSY` 在 250 ms 内每 5 ms 等待后再提交，其他拒绝立即失败；已被接受的事件不重复提交。最终 200 次入队的返回码均为 ACCEPTED，BUSY 次数为 0，最长入队调用 61 微秒。较早失败轮没有保留具体拒绝码，不据此声称已确定生产故障根因。

该调用方源码摘要为 `88fe66ae52610e4f5adda46356feb16fb8477ee869f3daa84758cd711de83249`，生产源码文件与上述公开 Base 相同。官方 RSA v2 验签 app 仍为 1183744 B，摘要 `deb7f0b0a3e59625dcad3fc09438fc16a1dc72341303543fb19843b85a51e6cd`；完整 4 MiB 实验 Flash 摘要 `58230944652709fa00a5f1baff6c3630b69fd57fe22d02ceb28550fa2bf8e2f1`，写后整片读回相同。UART 日志摘要 `6c7ab51913dcb9cd10a6af1449f4db58a18f752cbbed31d246e86ab5f0736f5c`，完整 100 轮和最终成功标记均已取得。

| 入口 | 次数 | stop／close／join 最小／中位／最大微秒 | 成功事件计数 |
| --- | --- | --- | --- |
| event | 100 | 658／688／865 | 每次 0 |
| timer | 100 | 731／824／878 | 每次 1 |

| 完成轮次 | 回收后可用堆 B | 历史最低堆 B | 回收后最大块 B |
| --- | --- | --- | --- |
| 1 | 201308 | 98776 | 114688 |
| 10 | 201320 | 98776 | 114688 |
| 50 | 201320 | 98776 | 114688 |
| 100 | 201320 | 98776 | 114688 |

百轮回收后的可用堆范围为 201308–201320 B，首末增加 12 B；历史最低堆 98776 B、回收后最大块 114688 B 均保持不变。这是当前无网络切片的 MCU 堆观察，不包括安装／卸载循环、全部任务／socket／计时器计数或五能力峰值。实验结束再次清空数据、逐字节恢复原三份代码制品，确认 Wi-Fi 关闭和串口释放；没有 eFuse 写入。

## 尚未闭合

宿主 Flash／NVS、固件集合与任务边界使用测试替身。C3 已完成实际 event／timer 原生停止及百轮回收切片；公开管理面、init 取消、联网组合动态资源、迟到异步回调、实体掉电、72 小时负载与 ESP32 实板尚未验收，P6-07／P6-09 保持进行中。当前只有 C3 接线。
