# 开发检查点

2026-10-03 C3 FRP关闭阶段复验：同来源签名固件、新Go观测与36纯检查／70成员输入独核后上板。最大64KiB／同boot单Pong及移除后80次status unconfigured；ACK短暂network_unavailable。自然退出循环再次失败，新增日志关键返回均在SIGTERM取消后，最终FINISHED／exit0不算自然。wall约2.579秒、erase约619毫秒，仅有限读数；原三码恢复／擦数据和UART／监听释放通过，90成员全档保留。整体成本资格false、节省0；关闭原因、容量与双目标／交付继续，见[分配诊断检查点](ota_allocation_diagnostic_checkpoint.md)。

2026-10-03 C3 最大FRP控制记录有限实测：实际65536 B单记录与同boot单Pong，wall约2.565秒、整区erase约564毫秒、读1179648 B及native失败0；公开移除后84次只读status为unconfigured，但夹具自然退出十秒联合门未过，整轮与成本总体资格保持失败。两fresh基线、原三码恢复／擦数据、UART和实际listener释放通过，原失败完整保留；普通Pong沿用已有4096 B窗口不擦scratch，不按心跳计算寿命。软件新A/C签名构建及最终输入通过，DRAM观察成本＋80 B、实际节省0；容量、争用、ESP32及交付继续，见[分配诊断检查点](ota_allocation_diagnostic_checkpoint.md)。

2026-10-02 C3 配置所有权：长期 context 的 7640 B 移入 RTC，启动先清空再从 NVS 恢复；临时 7618 B 编解码 owner 按需申请并清零释放，ESP32 保持原策略。最新精确依赖下双目标普通／签名构建、官方验签及完整 host 回归通过；C3 一次 WRITE 联合 OTA、MQTT restart、十二项业务、卸载和原代码恢复通过。124 份下载采样均 MQTT／FRP ready，来源历史 heap 23800 B 仍低于 48 KiB；完整容量未通过，见[RTC 检查点](rtc_config_ownership_checkpoint.md)及[前轮 MQTT 消费](mqtt_owned_config_consumer_checkpoint.md)。

2026-10-02 命令载荷按类型分配、串口缓冲按实际长度增长并清零回收，已准入配置转交试运行 owner；两个目标控制栈统一 8192 B。双目标签名／官方验签及 ASan/UBSan 回归通过，C3 一次 WRITE 联合 OTA、MQTT restart、十二项业务和恢复通过。141 份下载采样均 MQTT/FRP ready，控制栈最低余量 2400 B；历史 heap 11404 B、采样连续块 16384 B 仍未达门，见[命令内存检查点](c3-command-memory-checkpoint.md)。

2026-10-02 C3 稳定 FRP 身份联合功能切片通过：公开一次 WRITE OTA 与一次 MQTT restart 后，目标和第三 boot 均经 FRP 认证状态核验，十二项消息计数业务、卸载、A／C 字节核对及原代码恢复完成。143 份实板证据冻结；来源下载最低历史 heap 4124 B，48 KiB 容量门继续失败，见[检查点](c3-five-capability-run-id-checkpoint.md)。

2026-10-02 FRP 稳定请求身份接线：已有设备 UUID 同时用于 client_id 与 run_id，公开组件 0.2.0 的精确提交及 SDK 生成的双目标锁已更新。两目标签名构建、官方验签和 host 回归通过；C3 联合重启恢复与容量继续复测，见[检查点](frp-stable-run-id-checkpoint.md)。

## 2026-10-01 固件根与宿主工具归位

全工作区检查发现固件 README 标准入口漂移，以及主机 Python 构包工具位于固件根。现修正唯一标准链接，将 `container_product_deadline_test.py` 和 NVS QEMU runner 分别归位到 `tools/container_product_deadline_test.py`、`tools/run_nvs_capacity_qemu.py`；生命周期 Shell 入口和 NVS 探针三阶段命令同步使用新路径，旧文件删除，不保留别名。两个脚本的路径推导仍分别绑定本仓固件测试源码和本仓完整 Flash 排除边界，宿主与执行位保持原合同。

独立 Git tree 冷导出与固定 SDK 宿主完成 C3／ESP32 两个目标的完整真实签名 guest 生命周期入口；ASan/UBSan、期限／异常退出、各 100 次重装和 macOS 资源版通过。两个资源版本在第 10／50／100 次的 malloc、虚拟字节和 region 读数各自相等。190 个未改的固件／C 测试非文档输入逐项保持字节与执行位，SDK、Container `52d94d6`、WAMR `c10736f`、OTA `04acb5e`、依赖锁与分区未改，无需因宿主归位重签固件。QEMU runner 帮助、两脚本编译与真实仓内 Flash 路径拒绝通过，未因此重新宣称 NVS QEMU 容量验收。

隔离中央工程检查还修正了嵌套 ESP-IDF 测试项目的 `managed_components` 上游辅助脚本误报。原检查器明确拒绝该正例；修正后根与嵌套生成目录通过，同一测试项目旁边的第一方 Python 仍拒绝，当前 Base 嵌入式标准检查通过。共享停写窗口内中央检查器只保存在隔离候选，canonical Root `0ee90ab2` 的全工作区失败记录保留，尚未据局部结果改称全量门禁通过。

共享停写解除后的后续检查：本归位修正已合入并推送 canonical `master`，中央检查器已在主工作区应用。`report_template_drift.sh --all` 的工程标准、源码模块和产品身份均为 `CLEAN`，源码模块 33/33 通过；工作区控制文档 830/830、3,510 处引用无缺失。原全量失败与隔离检查记录保留其时点范围，不替换为事后通过；当前结果仍不代表正式发布或实体板验收。

本轮没有实体串口、Flash/eFuse、生产信任材料或发布操作；C3 仍为此前已清空的 MQTT 诊断样例，ESP32 未连接。真实网络、设备安装、产品停止跨重启策略及 P6/P7/P8 实板门保持开放。


## 2026-10-01 源仓历史与主机入口整合

隔离产品链候选整合源仓 `135bb87fd22925c876a3423285ac44fb37dd558d`，保留已验证的双目标独立分区、V3 联合 OTA 收据、Container 装配与配置上下文保护。所有固件执行输入与整合前 `b55241395172e5475c691dda8be2fd90b79edcb1` 逐项相同；主机 Python 入口按源仓改为 `device_control.py`、`test_device_control.py`、`mqtt_lab_inputs_example.h`，三个真实读取器同步引用，不保留旧名入口。

维护者已明确允许丢弃 ESP 上的数据，当前任务无需保全旧业务/NVS 数据，也无需通过旧 v1→v3 原位迁移预检才能继续。当前只有 C3 接入电脑；C3 已按本轮设备证据全擦并运行 MQTT 诊断样例，ESP32 未连接。以下历史迁移和双板结果保留原时点范围，不表示当前设备仍运行旧 Base。此轮源码整合不含串口操作、刷写、生产信任材料变化或发布；实板联网、联合升级和容量验收仍未完成。

完整主机工具集在固定 SDK 构建宿主、独立 Git tree 归档和 `umask 077` 下 **47/47** 通过；SDK/lwIP 精确来源检查、改名后的公开 CLI 帮助和五个主机工具编译通过。首次本机全量检查缺 `IDF_PATH`，首次宿主检查又因权限负例夹具的目录模式受 umask 影响失败；现由夹具显式 chmod 建立公开目录，生产检查未改，本机严格 umask 的 11 项归档回归和宿主最终全量均通过。历史命令保留执行时路径，当前唯一 Python 入口见本节。


## 2026-10-01 FRP 设备认证重启

固定 status／restart 管理 listener 已硬切命名；restart 使用 USB／MQTT 共用写守卫和原 outcome，在认证 running 回执后按 100／2000 ms 边界执行一次重启。等待期间其他新写命令 busy，序列化失败不登记动作。两目标各 22 项 host 与两项实际 HTTP／HMAC 软件回环、固定 SDK 测试签名构建／验签通过；SDK、依赖锁、配置 schema 与分区未改。完整输入、镜像摘要与测试替身边界见[FRP 认证重启检查点](frp-restart-checkpoint.md)。

尚未接入私有网关重启账本与新 boot 确认，真实外侧 HTTPS／FRPS、MCU 密码、两板及资源门槛继续未验收；本轮没有设备写入或生产发布，共享 Root／Profile／gitlink 保持停写。

## 2026-10-01 FRP 独立只读状态与响应认证

设备 status 请求现严格四字段，不需要 USB／MQTT 先提供 boot／uptime；同 ID 的服务端 30 秒首次快照缓存保持八槽，新查询使用新 UUID。所有非空认证响应由实际 PSA wrapper 签发原始 JSON HMAC，错误输出长度或密码失败不发送成功结果，写命令合同保持。双目标完整 host、OpenSSL 回环 HTTP 及已有产品事件 probe 通过；固定 SDK 产品签名构建、官方验签和槽容量通过，两个 ELF 中均已链接新签发代码。完整输入、镜像摘要、早期缓存／测试同步故障及验收边界见[FRP 认证状态检查点](frp-authenticated-status-checkpoint.md)。

未接私有网关、外侧 HTTPS 或真实 FRPS；未验证 IDF 密码运行、五能力并行与两板。共享合入／发布冻结保持，仅 C3 连接，未读写实体配置、Flash 或 eFuse。

## 2026-10-01 已确认 guest 异常退出的回收边界

真实 RSA-3072/PSS 签名的 `event-loop` guest 在 WAMR 中执行事件并触发预算异常；新增候选准备前／后的两项宿主场景。准备前异常：升级准备拒绝、返回零序号，NVS／Flash 写与擦除计数不变；准备后异常：旧实例不能被普通停止入口当作成功停止，trial 与同 boot 重开均阻断，未运行候选。只读状态无活动实例且原确认包摘要保持；测试以精确原操作显式调用内部放弃候选，读取 ABORTED 序号与旧绑定，再执行精确卸载。卸载等待实际 pthread 退出，证明 native 回收，当前绑定清除后可进入 EMPTY，整个包分区逐字节不变。

Base 仍精确消费 `esp-container@52d94d696d4cb0de3ce6a037c844c16be7edfb54` 与 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`；SDK、生产源码、协议、锁与分区均未改。C3／ESP32 两种目标定义各自执行完整 ASan/UBSan 生命周期入口，已有 deadline、trial 异常、联合 OTA 异常回退及各 100 次重装继续通过；macOS 资源版既有循环也通过。无需因测试变更重新构建或重签未改变的固件。

种子夹具通过真实签名与槽 API 建立确认绑定，未经过公开安装请求、代表事件或 30 秒健康确认。Flash/NVS、调度与固件观察仍为宿主替身。公开 `product_task` 在准备完成后普通停止失败时，当前保持 unknown 与本 boot claim；本用例的内部显式放弃不是公开恢复能力，也不授权发布方重发。公开停止／启动尚未实现，其是否跨重启保持停止等待维护者裁决；没有选择默认值、引入持久启动策略或改写现有重启行为。两块实体板与生产链路未验收，仅 C3 当前连接，没有串口、复位或 Flash/eFuse 操作。

## 产品事件真实 TLS Broker 宿主续验

公开发布器生产实现保持 `b8e9c791f70440209a6d1a416aad2cf69d1df7e7` 的原内容。本次增加 `firmware/tests/product_event_broker_probe.c`；外部 Broker 消费者的测试显式传入 Base checkout，运行固定 Paho 2.1.0 发布器，并将真实收到的字节交给本仓实际 wire 解析与授权包装源码。PSA 测试接口通过 OpenSSL 计算真实 HMAC，替代 IDF 密码端口；以 ASan/UBSan 编译。Broker 从官方 Mosquitto 2.1.2 精确源码 `99fa50f30e325609394c324c8ff71cfbbe95d8ab` 原生构建，加载同版本认证与 ACL 插件，只绑定本机回环临时端口，使用虚构设备／账户及公开测试管理密钥。

16 项通过：合法事件及完整 4096 字节帧逐字节一致，只发布一次 QoS 1 非 retained；陈旧序号、错误密码、跨设备控制方、仅 retained reported、错误 TLS hostname 在发布前拒绝；PUBACK 后无执行结果、断线、换 boot 或摘要不符均 unknown，不重发；通过 Broker ACL 的坏 MAC、旧 boot 和 QoS 0 被真实 C 解析器拒绝。业务失败和 runtime 失败分别保留负结果与 null，未当作成功。增加解析器后的首轮断线测试在子进程完成前断言，发生测试同步竞态；改为解析完成后断线，保留失败与最终结果。

reported／guest 结果和在线状态仍是明确假件。没有运行 MCU MQTT 传输、Base owner、高水位准入、Container 队列、真实 guest、30 秒在线或持久产品确认；最大帧只证明 wire 上限，不代表签名 guest 的事件配额。新 probe 不进入固件镜像，不改 SDK／五仓锁、分区、生产凭据或任何实体设备。生产事件源与两板链路、P6-04/P7/P8 继续未验收。

## 2026-09-30 活动产品完整版本与试运行回读

`product.status` 现硬切为十个 required 字段，新增 required nullable 的七字段 `active_product`。实际实例从本次 `econtainer_product_open` 成功后的复验 manifest slice 复制完整版本；产品 ID 与不可变编译授权逐字节核对，版本按实际长度申请。发布、查询副本与停止清理由原事件锁保护；停止、trap、回退和卸载不保留活动版本。查询复用原同一 claim、双次签名固件观察和一次 ECS2 读取，确认实例匹配确认绑定，产品 trial 匹配本 boot 的精确候选；协议再核对原未决账本 ID／包摘要／操作类型。确认包根字段在 trial 期间保持旧绑定。联合固件 pending trial 的占用和观察限制保持，不能据元数据宣称健康或原操作成功。

版本副本分配失败清空两个视图并返回资源失败；其他观察或绑定不确定继续阻断。写入预检的绑定快照不分配版本。响应按实际长度申请，无固定 4 KiB 常驻状态区；保守完整字段／最大 uint32／4096 字节身份正文的 JSON 框为 5059 字节，5120 字节出站边界容纳完整内容。MQTT 入站上限与三消息槽仍为 4096 字节，出站由官方 outbox 复制、限额与释放，增加的瞬时堆占用尚须实板测量。

Base 精确消费 `esp-container@52d94d696d4cb0de3ce6a037c844c16be7edfb54` 和 `esp-mqtt@a46e209cc98c7b910774dbb77d11b34f79492720`，SDK、lwIP、WAMR、OTA 与 FRP 锁保持。Component Manager 在独立候选上分别生成产品与 NVS 探针的四份目标锁，未手改生成摘要。初次默认锁更新未切换 ESP32 自有锁，旧组件编译明确拒绝缺少新字段；分别重新解析对应目标后两目标通过。NVS 探针仅完成 stage 3 配置与依赖解析，不计作新 QEMU 或实板容量结果。

双目标签名构建、官方 RSA v2／ECDSA v1 验签与槽容量通过。两目标完整 host ASan/UBSan、真实签名 guest 生命周期／deadline／trap／资源版及各 100 次重装通过；新增断言覆盖独立版本夹具的 3000 字节版本、候选→确认、同 boot v1/v2、回退到旧版、停止后的空视图、活动摘要冲突与副本分配失败。公开 CLI 15 项通过；协议与 MQTT owner 回归逐字节返回超过 4096 字节的完整状态并拒绝出站超限。MQTT 独立 host ASan/UBSan、工具 8 项与 Linux 真实核心／隔离 Broker 的 5120 字节 QoS1 原 ID／DUP／原载荷重传通过；Linux 场景不代替设备 TLS／Broker 验收。

208 个构建／测试快照输入与候选逐项 SHA-256 一致，组合指纹 `d7e0825000ee080f1033c69b882b4e67b40fb43ec433dc4dcd8a6a2a578ac26c`。测试签名键沿用此前仓外夹具，未使用生产键或改变设备信任根。

| 目标 | 测试签名 app／槽容量 | 完整 app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `9885f68a8da5a5fa89481a9558a696b1560261edf18dde8b0b28f639666b4f88` |
| ESP32-D0WD-V3 | `0x10fff4/0x120000` | `326d97716823e61496068b5664742e999b2c38021729cc4c28d99c8d464d7d4b` |

Tool 已严格贯通 Swift／Go／HTTP／Web，并完整展示活动版本和候选状态，Bruno 与 OpenAPI 同步。当前仅 C3 连接，ESP32 已拔除；此轮未打开串口、复位、写 Flash／eFuse或部署正式环境。C3 旧非空未知持久区保留范围仍待维护者裁决；两板设备回读、五能力峰值、掉电、制品交付与正式发布继续待办，完整 P8-04 保持进行中。当前字段事实源见[设备协议](../design/device-protocol.md)，以下保留此前切片记录。

## 2026-09-30 产品状态的签名固件与绑定包元数据

公开 `product.status` 现严格返回九字段：原账本与绑定五字段，加运行固件完整 SHA-256、实际运行时 guest ABI，以及当前确认包的 guest ABI／数据 schema。固件摘要直接消费既有同一 claim 下双次核对的签名镜像观察，按 SDK 验签确定的完整镜像长度计算，包含 RSA v2／ECDSA v1 各自签名尾部；不是 ELF 或包摘要。运行时 ABI 来自实际编译的 Container 常量；包 ABI／schema 来自当前固件对应的 ECS2 确认绑定。无包时 wire 包摘要／ABI／schema 全部为 null；有包时两项版本均为正 uint32。查询不新增包读取／验签或 guest 健康证明，不把产品 trial 候选当作已确认绑定。

C3／ESP32 完整 host ASan/UBSan 与公开 CLI 15 项通过；严格客户端覆盖九字段、非零摘要、布尔／零／越界版本、缺字段及包元数据与绑定冲突。精确锁定 Container／WAMR 的两目标真实签名 guest 生命周期、deadline／trap、资源版及各 100 次重装通过；新增快照断言逐项对照实际固件观察、运行时常量和同一 ECS2 绑定。宿主 SDK／Flash／NVS／网络仍为假件，宿主资源观察不计为设备峰值容量。

固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c`、唯一 lwIP 合同、Container／WAMR 和两目标依赖锁未改。独立构建的 208 个已跟踪非文档输入逐项与候选校验一致。在独立仓外源码上使用完整测试产品授权、正式分区和一次性固件测试键完成双目标签名构建，官方 RSA v2／ECDSA v1 验签及 app 槽容量门通过。未使用生产签名键或改变设备信任根。

| 目标 | 签名 app／槽容量 | 完整 app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `aaeb81e8953b0121c7a5d8f2b2ea192e3bc547ae4f3b35ca1be8de33c7384c5b` |
| ESP32-D0WD-V3 | `0x10fff4/0x120000` | `833abe5aa07f24e976d7ffa67fdbfe78dea974f66089ac6353a3321c22955c96` |

构建输入及签名日志保留在仓外独立临时目录。首轮配置因验证脚本误填包分区标签而被真实 CMake 门拒绝；改回正式 `product_pkgs` 后构建通过，源码合同未放宽。ESP32 初次官方验签调用使用了不支持的私钥格式，随后导出同一测试键的公钥复验通过，镜像字节不变。两次验证输入错误均保留原始日志。

当前实体仅连接 C3，ESP32 已拔除；本轮未打开、复位或写入实体串口。此软件切片不证明正式 USB／MQTT 回读、真实 Broker／FRPS／HTTPS、五能力峰值、掉电或迁移成功。实际活动产品版本、trial 展示和两板联合验收仍待闭合，P6／P7／P8 不因此关闭；C3 旧非空未知持久区保留范围仍待维护者裁决。精确公开字段见[设备协议](../design/device-protocol.md)。

## 2026-09-30 C3 带包离线 pending／回退 UART 诊断

从 `eb41a4a02a00adb40c9102c2e20f4ae09dfa68d1` 完整 Git 归档建立独立仓外副本，归档 SHA-256 为 `a38159cc8511f4704ef6932bb7085aade0807f20471e955a371ca17018fbc5cb`。逐文件比较仅有两个已跟踪文件差异：`esp_base_protocol.c` 将 C3 的控制台 VFS 分支改接所选 UART，`device_protocol/CMakeLists.txt` 将该诊断的驱动依赖改为 UART。独立 sdkconfig 选择 UART0；正式 USB 源码未改，没有加入测试任务、业务函数调用或健康替身。SDK、精确依赖锁、C3 正式双 `0x130000` app／三份 `0x77000` 包槽／scratch／十一页 Base NVS 均沿用上一检查点。

第一次诊断遗漏仓外 UART 驱动依赖而编译失败，修正后日志单独保留。首次 QEMU 又因附加全零 eFuse 文件报告芯片 v0.0，与签名镜像最小 v0.3 要求不符，在应用前退出；没有降低镜像版本门。根据[该固定 QEMU 的官方 eFuse 实现](https://github.com/espressif/qemu/blob/esp-develop-9.2.2-20260417/hw/nvram/esp32c3_efuse.c)，最终不附加 eFuse drive，使用模型原生 v0.3／eFuse block v1.3；全零输入失败保留在 `first-zero-efuse-startup/`。官方 espefuse 的虚拟模式另生成的诊断文件未被 QEMU 消费，也未连接任何实体端口。

C3 QEMU 仍缺 ADC2 校准外设，GDB 在 `*adc2_init_code_calibration` 的第一条指令前跳转至调用方返回地址，避免执行函数序言、保持 SP；除此之外只在真实 app_main 返回后读取主 TCB／填充字节。此差异使结果只属于 UART／模拟器诊断，不是正式 USB 镜像或物理外设证据。签名 app 字节没有被 GDB 改写，未注入网络就绪、时间、代表事件或健康提交。

| 输入 | 大小／槽容量 | 完整 SHA-256 |
| --- | --- | --- |
| UART 来源 A | `0x121000/0x130000` | `9a153f15ffc7ccfcc3b0b5f9df862040ab9faef0ff4f7da7d26938c354b81631` |
| UART 候选 C | `0x121000/0x130000` | `1c18c12bae04a97202537b495e63a3f3b92647f64850e3a5b29b0280cca7750a` |
| A sdkconfig | 88,112 B | `6cfcdc904819e1ef4da6425e475b5515293beaf45d5486e3bc234c2bfb4a9b0b` |
| C sdkconfig | 88,139 B | `e28d62cb504b970bc26998112dab303252fa1cd3767c932a84051448419ab7bb` |

A／C 分别采用 SDK 原生产品版本 `0.2.0`／`0.2.0-qemu-c`，两镜像官方 RSA v2 验签及 app 尺寸门通过。完整合法测试授权、真实签名 P0／P1、合成原 V3／ECS2／EPRD 和三阶段执行方法与下节 ESP32 一致；seed 改为 C3 正式几何，以 host ASan/UBSan 验包并构造来源 sequence 6、REUSE／WRITE 的 PREPARED 7／8。代表事件和安装健康前置仍是合成事实，不能认定公开安装、下载或 MQTT 交付通过。

两种模式的正式业务路径均完成：候选启动持久 trial sequence 8／9，跨本地稳定窗口的三个 15 秒间隔查询继续返回原 ID running、原包模式和摘要；Wi-Fi／可信时间／MQTT 均未就绪，配置写入持续拒绝。以该 Flash 新启 QEMU，由 SDK 回到 A，Base 退役 C、恢复旧 P0，原 ID 返回持久 FAILED，然后到 READY；再次冷启动仍 READY、P0 与原失败结果不变。REUSE／WRITE 回退主栈最低未用均为 2,260 B，二启均为 4,244 B；配置值 6,144 B、SDK 实际分配 6,656 B、TCB 对齐跨度 6,641 B 分别记录，不混为一项。

官方 NVS parser 校验身份 NVS 与 Base NVS 全页／条目 CRC，ECS2／EPRD 内部 CRC 通过；回退后 sequence 10／11、IDLE，来源绑定和原产品账本逐字节保持。原 V3 除 state／失败码两个字节外所有字段与前置收据一致；同样的完整原字段断言也补核了下节 ESP32 读回。C 首个 4 KiB 扇区被擦除、其余 app 字节保持，inactive otadata 全扇区退役。bootloader、分区表、身份 NVS、phy／coredump、A、包区和 scratch 逐字节不变。两模式相对初始片分别只有 otadata、C、Base NVS 的 12、4,093、1,419 个字节变化；4,093 是原 C 首扇区非 `0xff` 字节数，不是擦除长度。

回退与二启的完整 4 MiB Flash 逐字节相等，REUSE SHA-256 为 `d8e9a42efed019a78ca2b9aa9d409f6f94255eb59587ab2016244b3cd832ed01`，WRITE 为 `e1ce1bb8faad77052a4a77c07607dfa7f1964b1e4f41631395fb83d880b99cb8`。这证明内容幂等，未测物理擦写次数或寿命。已有 status 采样中的普通堆历史最低为 98,556／98,508 B，属于离线 UART 仿真，不计为五能力资源验收。

全部输入、源码差异摘要、构建／官方验签日志、`seed.c`／准备与运行脚本、GDB／UART、完整 Flash 与 `verified-results.json` 位于 `/private/tmp/esp-base-eb41a4a-c3-pending-qemu-20260930/`。QEMU 版本与下节相同，禁自动重启、关闭模拟 timer WDT。当前实体只连接 C3，本轮未打开、重置或写入实体串口；正式 USB／真实 Wi-Fi／Broker／FRPS／HTTPS、联网成功确认、迁移与物理掉电均未验收，P6-03/P6-10/P7-04 保持开放。

## 2026-09-30 ESP32 带包离线 pending／回退启动与主任务栈

在 `85e8e71af21d7fac42c85d78eccc66933e59835c` 源码上补齐完整、合法的仓外 Container 测试授权后，正式 ECDSA v1 签名 ESP32 app 在两种带包回退启动中都复现 `***ERROR*** A stack overflow in task main`。当时主栈仍为 SDK 默认 3,584 B；此前 `0xffff4` B 的 ESP32 构建使用空产品授权，只证明该配置的装配，不能证明完整产品路径容量。故障沿实际 app_main、签名固件观察、Container 放弃／删除迁移和 NVS 持久恢复发生，没有 GDB 业务函数调用。失败输入和原始日志保留在仓外 `third-run-3584-byte-main-overflow/`。

本次最小修正将已有 C3 的 6,144 B 主栈移到两目标共用默认，并将普通 Base 的构建下限扩到 ESP32；C3 实际配置不变。该 SDK 的非 nano 格式还增加 512 B，实际分配 6,656 B，TCB 的对齐后高低地址跨度为 6,645 B。静态帧取证显示 Container 持久化、放弃和删除帧分别为 896、624、624 B，另有 Base 与 SDK 调用链；单帧之和不能代替运行峰值，因此重签应用并完整重跑两种模式。

固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 和唯一 lwIP 合同检查通过；Container `e8a0d0b6384bbba813b955ed08ebc315c134a707`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 及所有组件锁未改。使用正式分区几何、UART0、回滚和完整 Container 授权，app 源码没有加入探针或网络替身。A 使用产品版本 `0.2.0`；C 仅用 SDK 原生 `CONFIG_APP_PROJECT_VER_FROM_CONFIG` 设置 `0.2.0-qemu-c`，使两个实际签名镜像身份不同。测试授权为 `test-product/test-key`，一页 Wasm、16 KiB owner 栈、事件队列上限 4、最大 guest 入口 100 ms；临时包签名键及固件测试键均不入仓，也不是生产密钥。

| 输入 | 大小／槽容量 | 完整 SHA-256 |
| --- | --- | --- |
| ESP32 来源 A | `0x10fff4/0x120000` | `10b701726c9859b0686c952bd901c2d88f03867b559fb222615d2dbe78322143` |
| ESP32 候选 C | `0x10fff4/0x120000` | `6860d0bebfca0c7b4514e883f5a436f87564a1939ac843be4526c1a0d7dcfe0f` |
| C3 正式 USB 产品构建 | `0x121000/0x130000` | `35f57204b0b3c26e8cf9ad3b00cd8eb33347a8c8b90e21d212b935691d000aa6` |
| 来源 P0 counter.pkg | 10,240 B | `2fb57a90a325ec3b09da47e4ddc3792fa72e7f2b1c57fe6e1c98cd89d4eea38c` |
| WRITE 目标 P1 counter-v2.pkg | 10,240 B | `328092a41690e117ec6a5efbdb7df83491c9a2e36484d0c6450a723b10bb66cb` |

双目标完整授权构建和官方 RSA v2／ECDSA v1 验签通过，ESP32／C3 配置 3,584／6,143 B 的独立实际 reconfigure 均被共同主栈门拒绝。C3 本轮只验证正式 USB 签名构建，没有运行同一带包恢复场景。

仓外 `/private/tmp/esp-base-85e8e71-pending-qemu-20260930/` 保留 `seed.c`、`prepare.py`、`run.py`、`verify.py`、完整配置／ELF／签名 app、包和逐阶段收据。host ASan/UBSan 的 seed 通过真实 Container 验包／槽 API 合成 P0 已确认 sequence 6、原产品账本及原 V3 意图，REUSE／WRITE 分别预置 PREPARED sequence 7／8；代表事件摘要为测试字节摘要，健康前置由 seed 构造。官方 NVS generator 将 ECS2、EOTA V3、EPRD 与固定合成设备 UUID 写入新 4 MiB Flash，A 为 VALID，C 为 NEW。它不证明公开安装、公开下载或代表事件已在设备发生。

每种模式分别运行同片的三次独立 QEMU 进程：候选 pending、复位回到 A、再次启动 A。实际候选 trial 持久推进到 sequence 8／9，跨本地检查窗口和三个相隔 15 秒的查询继续返回原 ID `running`，包模式／摘要正确；无 Wi-Fi／时间／MQTT 就绪事实，固件未被确认，配置写入返回 `ota_verification_pending`。下一次 SDK boot 回退到 A，正式启动退役 C、恢复来源 P0、将原收据持久记为 FAILED 后到 `READY container=running`；再启动仍返回同一失败结果。GDB 仅在实际 app_main 返回且主 TCB 尚未删除时读 RAM 填充值，没有调用函数、伪造时间或注入健康。REUSE／WRITE 回退主栈最低未用分别为 2,324／2,308 B，二启均为 3,780 B；均超过本切片 1 KiB 初始观察门。

官方 NVS parser 的完整页／条目 CRC、ECS2／EPRD 内部 CRC、原 V3 的 308 B 长度及字段读回通过。回退后的 ECS2 sequence 为 10／11、IDLE，来源绑定与产品账本逐字节保持，候选绑定删除；A 仍 VALID，C 的首扇区和 inactive otadata 扇区按真实 SDK 退役，C 其余字节保持。bootloader、分区表、身份 NVS、A、整个包区、旧 AT 保留区和 scratch 均未变。回退与再次启动的整片 Flash 逐字节一致：REUSE 为 `6643f741ec21bd72719e7c08254bfb919516be14afa7cf8565f82eae60932698`，WRITE 为 `1d668b8dcdbdffc3fc78dbe0c95d5e461005ad729cc728970112be9f6f84d5f0`；各模式相对初始片仅 otadata、C、Base NVS 改变 12、4,096、1,426 B。

QEMU 固定为 Espressif `esp_develop_9.2.2_20260417`，禁自动重启并关闭模拟器 timer WDT；没有真实 Wi-Fi、Broker、FRPS 或 HTTPS 会话，没有验证联网成功确认、公开 worker 从下载开始的完整链或物理掉电。普通堆历史最低约 119 KiB 属于离线 UART 场景，不能回填五能力容量。首轮 120 秒启动期限不足和第二轮测试客户端读取错误字段的失败日志也保留；最终采用 360 秒仿真启动期限和实际 `revision` 字段，产品期限未改。维护者已确认当前只有 C3 连接、ESP32 拔除；本轮未打开或重置实体串口、未写板，双板发布范围与 P6-10/P7-04 实板／网络门保持开放。

## 2026-09-30 公开 REUSE／WRITE 联合 OTA worker

公开 `ota.start` 现消费三种包模式的准入合同。`NO_PACKAGE` 仍要求已准入 EMPTY；`REUSE` 要求已确认且运行的来源 guest；`WRITE` 可由该来源或 EMPTY 开始，任何 trial／blocked 状态均拒绝。worker 重读原 V3，逐项核对固件、包和代表事件字段，在旧 B 首次物理退役前再取得来源完整快照并核对原 A/B、ECS2 序号和来源包。Container 退役入口同步硬切为消费原收据，保留来源包引用并读回。A-only 已确认来源可保持原 CONFIRMED 相位／序号进入 stage 或 prepare 前恢复，不新增虚构持久步骤。

准备新固件成功后，来源有包时先停止 guest，证明 native 回收和线程 join，才进行包 stage。REUSE 重验原包并提交 PREPARED，无包 HTTPS 下载；WRITE 持久预约 WRITING 后使用既有严格 TLS、固定长度、无重定向、连续 offset、有界期限来源，完成写槽、完整摘要／签名／授权及 PREPARED 独立读回，SDK 完整传输也通过后才选择新 boot。HTTP/TLS 客户端在传输末尾释放。来源、停止、stage、下载尾部或 selector 不确定均保留原 claim 和 unknown，禁止新操作，后续由原 V3 启动恢复裁决；不把 partial C 或已完整包当作成功。活跃 worker 或 pending C 的原 ID 查询为 running，VALID 尚未持久成功仍为 unknown。

双目标完整 host ASan/UBSan 通过，覆盖原收据字段冲突与来源改变的无擦写拒绝、单／双来源就绪、来源停止失败、stage／传输／读回不确定、同请求不重执行、worker 创建失败、pending 写门与原 ID running/unknown。公开串口客户端 15 项通过。锁定 Container/WAMR 的双目标真实签名 guest ASan/UBSan、资源版、deadline／trap 与各 100 次重装通过；带包测试改用 Base 正式退役入口，覆盖 REUSE／WRITE 单固件和双固件来源、精确错误来源无提交、prepare 前恢复、候选健康与最终绑定。正向入队测试仅对未入队的 BUSY 做最多 200 次毫秒等待，负例仍一次裁决；生产队列和重发语义未改。

固定 SDK 与两目标精确依赖锁未变，双目标签名构建、官方 RSA v2／ECDSA v1 验签和槽容量门通过。链接映射确认 WRITE 续写已进入真实 app。签名 app／槽容量与 SHA-256：

| 目标 | app／槽容量 | app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `b3c66ae80123348bcae7b675974b51599cdb6e853f60ca420b0e734a3e9a021b` |
| ESP32-D0WD-V3 | `0xffff4/0x120000` | `dffc1c10da37f2ca0115130f3b1583bc6e2d33cd814504a197c89fcc0083202e` |

资源版 native malloc 在 10／50／100 轮均为 382016 B；macOS VM region 观察 C3 为 68／68／68、ESP32 为 66／66／68，属于宿主观察，不据此认定设备资源门通过。测试的 SDK、固件集合、Flash／NVS 和 HTTPS 传输仍为假件，MQTT owner 单测与签名 guest 分层验证；未在同一实际产品设备执行完整网络升级。正式串口／MQTT 全链、真实 HTTPS／Broker、P0→P3 连续更新、五能力资源、实体掉电和两板迁移仍未验收。本轮未写实体板，P6-10/P7-04 保持进行中。

## 2026-09-30 联合 OTA pending MQTT 与连续在线健康接线

普通主应用现保留原 V3 收据，在 selected C 双次固件／ECS2 预检通过后，持久本 boot trial 并启动候选 guest。仅此准入允许控制任务在 pending 写门内启用既有 MQTT owner；FRP 仍等待全部确认。控制任务复用产品 trial 健康谓词，采集请求绑定代表事件、包摘要及失败数，要求 Wi-Fi、本 boot 可信时间、MQTT 连续 ready 30 秒、采样间隔不超过 1 秒、提交时空队列／无在途调用。RAM 快照使用非阻塞原子锁，不在锁内做 Flash／TLS。失联、无代表事件、错包、失败数变化／溢出、时钟逆行或采样中断重算窗口；离线保持 pending，不提前确认。

主应用先完成本地控制进展窗口，再消费健康快照；Container 在事件锁下复核并冻结 guest，持久健康及独立读回后，Base 才确认固件并观察 VALID、确认包及引用，准备原产品账本，最后提交并读回原成功收据。提交前短暂忙可等待；健康提交不确定先停止并证明 native 回收才回滚。固件已 VALID 后包确认／账本／成功收据不确定，停止 guest 并保留原 claim 和写门。失败退出在控制任务内撤销 MQTT，清理未完可重试；成功转换保留会话。无包原有本地确认条件保持。

C3／ESP32 完整 host ASan/UBSan 通过。真实调度单元覆盖完整 30 秒重算、采样过期／锁忙、全部在线谓词、计数溢出、退出清理与成功会话保留；主应用假件覆盖两模式晚到健康、未尝试提交重试、等待后 guest 故障、健康不确定与 native 停止失败，以及最终包确认、账本和收据故障。Container 本轮未改，真实签名 guest／持久冻结及回收证据沿用上一检查点；这些测试没有真实 Broker、FreeRTOS 并发或实体 Flash。

固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 检查、双目标测试键签名构建、官方 RSA v2／ECDSA v1 验签及容量门通过，链接映射证明 `start_firmware_package_trial`、`verify_firmware_package_health` 已在两目标实际 app 中。新增原收据静态对象 `0x118` B、RAM 健康对象 `0x48` B，另有两个原子字节及控制任务状态；静态链接不证明运行峰值内存。

| 目标 | 签名 app／槽容量 | app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `59a72d8234e0cb0421da6eeb13fdd1f016e05cfddb1648a164896e6530339085` |
| ESP32-D0WD-V3 | `0xffff4/0x120000` | `6ff848e5c5263f94674ad6c049434cbc83d690298e4ecff33228fa5f9e37b58f` |

公开带包下载 worker 仍在持久登记前拒绝，带包 PREPARED 查询仍返回 unknown，待实际公开事务接通后统一开放；本轮不证明端到端公开 `ota.start`。SDK／Container／WAMR 和两目标依赖锁未变，未写实体板。真实 MQTT 授权事件、HTTPS 下载、五能力负载、Flash／otadata 掉电与两板迁移仍未验，P6-10/P7-04 保持进行中。

## 2026-09-30 联合 OTA 内部带包 trial 与健康持久提交

内部 `start_firmware_package_trial` 现只消费原 `PREPARED` V3 收据，双次观察 pending C 与回退 A 并精确核对原操作／模式序号后，先持久 `TRIAL_STARTED` 和本 boot，再验签、授权和启动候选 guest。产品专用与联合固件包 trial 共享执行器事件跟踪：错误事件不建立代表完成序号；业务、运行时、定时器或日志失败撤销代表证据并计数。`verify_firmware_package_health` 要求调用方先取得连续在线健康窗口，再在事件锁下复核原代表事件、包摘要、失败次数及空队列／无在途调用，冻结事件和定时器，持久记下健康并独立读回。`NOT_STARTED` 证明未尝试持久写入，可等待下一控制轮；任何已进入提交的未知结果均保留冻结及长 claim。

固件须随后由 Base 确认并观察为 VALID，内部 `confirm_firmware` 才提交包绑定并再做完整引用对账，成功后撤销 trial 和冻结、恢复事件入口。确认不确定保持冻结；真实 trap 已结束且 native 回收、线程 join 的联合 trial 可安全进入固件回滚，无回收证明仍拒绝。无包成功确认也清理临时 trial 状态并允许同 boot 后续产品操作。

真实精确锁 Container/WAMR 的双目标 ASan/UBSan 和资源版签名 guest 回归通过，覆盖空来源 WRITE、有来源 REUSE／WRITE、错事件、错序号／失败次数／事件摘要、提交期间拒绝事件、健康读回不确定后 A 回滚、最终确认读回不确定后的 VALID C 只读恢复、真实 guest trap 的 native 回收及两种模式 A 回滚，以及各 100 次重装。测试对暂时繁忙的非阻塞事件快照做有界重试，保持原预期字段断言。宿主固件身份／otadata 与 Flash/NVS 是假件；测试直调健康入口，不模拟在线 30 秒或真实 MQTT 授权。

固定 SDK 检查、C3／ESP32 完整 host ASan/UBSan、既有测试键签名构建和官方 RSA v2／ECDSA v1 验签通过。当前 app 为 `0x121000/0x130000`、`0xffff4/0x120000`，SHA-256 分别为 `106026307612ed39094626e149ec147e77bb91176dc6e70daede855e2af3f6c2`、`97d92ea1b2221fb7bca5f61a6d0bb3933b20a19a63539ceb4d76df81fc93022e`；SDK、Container/WAMR 与两目标锁未变。新内部入口尚未被主应用消费，可能被链接器移除，这组尺寸不证明公开联合成功链的最终容量。

普通启动仍阻断新 pending 包 trial，公开带包 worker、pending MQTT owner 与在线健康窗口仍待接线。本轮未写实体板，P6-10/P7-04 不验收。

## 2026-09-30 联合 OTA 带包 C 侧 VALID 启动恢复

普通启动在本地存储、身份、安全、配置与控制进展检查通过后，现可消费原 V3 收据恢复已 VALID 的带包 C。Container 核对 A/C 签名集合、原 operation、包摘要／长度／ABI／schema、来源绑定及模式对应的精确 ECS2 序号；只有原 `HEALTH_VERIFIED` 加旧 trial boot 或同操作 `CONFIRMED` 才能通过。持久确认并独立回读后才启动已确认 guest、准备账本，再提交和读回原收据 `SUCCEEDED`。`PREPARED`／`TRIAL_STARTED`、错误收据、包损坏和任一步不确定均保留启动 claim 与写门。历史 `SUCCEEDED` 允许后续合法产品操作推进序号，但仍要核对原 A/C 集合和真实包引用；未决收据不得借此绕过原操作约束。

双目标完整 host ASan/UBSan 通过，覆盖 VALID 恢复接线、基本控制检查失败、成功收据不确定与原操作终态查询。真实锁定 Container/WAMR 双目标签名 guest 生命周期通过：无来源 `WRITE`、有来源 `REUSE`／`WRITE` 的确认恢复，确认读回失败后的只读重试、来源保留、包字节损坏、后续产品操作与各 100 次重装。健康状态在该恢复测试中由 Container 状态原语构造，用于检验跨提交裁决，不证明真实 MQTT 代表事件或 30 秒在线窗口。收据两模式覆盖 NVS 写前、写后、commit、独立读回与错误 selector；终态字段和来源／代表事件摘要保持原值。

固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 检查及现有双目标锁通过，仓外既有测试策略、测试键签名构建与官方 RSA v2／ECDSA v1 验签通过：

| 目标 | 签名 app／槽容量 | app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `cd502a57577145c5f01b6fe1f59c1940919c582a7a89a89892cf856e86f573de` |
| ESP32-D0WD-V3 | `0xffff4/0x120000` | `7e1183e2be8d1eda296d0a9f975bcc15ef85f76c381bf01ef753c2a8c1d84799` |

本轮没有写实体板。公开带包 worker、新 pending C 的授权代表事件与连续在线联合确认仍未接线，实板掉电及五能力负载未验，P6-10/P7-04 保持进行中。

## 2026-09-30 联合 OTA 带包 A 侧启动恢复接线

普通 `app_main` 不再一律拒绝带包 V3。A 仍为运行且 boot 指向 A 时，原收据限定目标 app 的物理退役；Container 再核对原来源包和 A/B/C 序号、放弃未完成候选并回读 A-only；最后持久提交并读回 `FAILED`，才进入正常旧包启动、产品账本准备与启动 claim 释放。任一 SDK／Container／收据不确定都阻止 guest 启动并保留写门。失败收据在下一 boot 只做无未决迁移对账，不授权再次擦槽。目标 C 的带包 trial 与成功提交仍明确拒绝，公开带包 worker 尚未开放。

双目标完整 host ASan/UBSan 通过。启动假件核对两种模式的原 V3 全字段传递及调用顺序，覆盖物理退役失败、ECS2 不确定、失败收据提交或读回失败、Container 策略不匹配、终态重复启动和未决绑定阻断；收据假件对两种模式覆盖写前、写后、commit、独立读回故障及错误 boot selector，逐项核对终态／unknown 和 Flash I/O claim 释放。真实 Container A-only 恢复与旧包完整性仍使用上一节已验证原语；本轮未新增其实现。

固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 及现有双目标锁检查通过，仓外既有测试策略和测试键下的 C3 RSA v2／ESP32 ECDSA v1 构建、官方验签及 app 尺寸门通过：

| 目标 | 签名 app／槽容量 | app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | `0x121000/0x130000` | `1b317cae709f08c3802a12234d1af41376b18bc48468637c616f408af9c821bf` |
| ESP32-D0WD-V3 | `0xffff4/0x120000` | `839d3e3d2219695892f7883f7ad11de2af42948d839f47532477fe6b3ffd7899` |

本轮未写实体板、未重做 Flash/NVS 掉电试验或五能力并发，未验证公开带包 OTA 成功路径；P6-10/P7-04 保持进行中。

## 2026-09-30 联合 OTA 带包 A 侧中断恢复原语

内部 `esp_base_container_product_recover_retired_firmware` 现在接收原 V3 收据而非分散的固件摘要。物理层已证明 A 为唯一可启动运行槽后，Container 层核对来源包摘要、长度、ABI、schema 与真实引用，再按原 operation ID、A/C 身份、模式和序号处理：旧 B 尚未在 ECS2 退役时先安全退役；`WRITE` 的 `WRITING` 或两种带包模式的 `PREPARED`／旧 boot trial／`HEALTH_VERIFIED` 经持久 `ABORTED→IDLE` 丢弃 C 绑定。`WRITE` 目标槽可保留部分或损坏字节，但旧 A 包必须完整；同 boot 的已启动 trial 不允许无 guest 停止证明直接放弃。每次提交由 Container 独立读回，最终 A-only 再做真实包引用对账。错误收据或来源包损坏阻断，不将其报告为已恢复。

C3／ESP32 host ASan/UBSan、锁定 Container/WAMR 的双目标真实签名 guest 生命周期各 100 次重装、固定 SDK 双目标签名构建和官方 RSA v2／ECDSA v1 验签通过；app 分别为 `0x121000/0x130000`、`0xffff4/0x120000`。测试覆盖空来源 `WRITE`、有包来源 `REUSE`／`WRITE`，B 退役前、部分 `WRITING`、`PREPARED` 和 `HEALTH_VERIFIED` 后的 A 回滚，来源损坏、错误收据、同 boot 阻断及旧包槽无额外擦写。普通 Base 启动仍在带包 V3 执行门前停止；该原语尚未被公开带包 OTA worker 或启动流程消费，物理 app 擦除前的联合预检、真实掉电和两板验收仍缺，P6-10/P7-04 不验收。

## 2026-09-30 联合 OTA 带包 selected C 只读预检

内部 `esp_base_container_product_reconcile_selected_ota` 在签名 `PENDING_VERIFY` C 与可回退 A 的双次物理观察后，按原 V3 收据只读核对 ECS2：必须为原操作的固件迁移 `PREPARED`，序号须精确包含旧 B 退役、stage，以及 `WRITE` 多一次的包写入提交。来源固件包身份须逐项等于收据；候选固件尚未绑定包，`REUSE` 指向来源包槽，`WRITE` 指向非来源槽。Container `reconcile` 同时复核真实包字节，只有决策为 `BOOT_START_TRIAL` 才通过。`WRITING`、错误操作或包参数、来源包不符与目标槽字节损坏均阻断，不写 NVS 或 Flash。

C3／ESP32 host ASan/UBSan、锁定 Container/WAMR 的双目标真实签名 guest 生命周期各 100 次重装，以及固定 SDK 双目标签名构建和官方 RSA v2／ECDSA v1 验签通过；签名 app 分别为 `0x121000/0x130000`、`0xffff4/0x120000`。新增用例包含无来源包 `WRITE`、有来源包 `REUSE`／`WRITE`、未完成 `WRITING`、收据与包字节篡改，断言预检不擦写。普通 Base 启动及公开 `ota.start` 仍在带包执行门前阻断；A 仍运行时的恢复、候选业务事件、30 秒在线健康确认、实体掉电与两板验证尚未闭合，P6-10/P7-04 不验收。

## 2026-09-30 联合 OTA V3 收据只读恢复字段

`esp_base_ota_receipt_load_for_recovery` 现在对格式完整且绑定一致的 `REUSE`／`WRITE` V3 收据返回原包模式、目标包摘要／长度／ABI／schema、代表事件摘要、来源包身份与固件 A/B/C 身份。旧 V1/V2 长度、畸形 V3、跨 schema 或字段冲突仍返回存储不确定；读取本身不写 NVS 或 Flash。Base 启动入口及 OTA worker 显式检查 `NO_PACKAGE` 后才进入已闭合的无包恢复／擦槽路径；合法带包收据在当前版本仍阻断启动推进。`ota.result` 对带包记录保持 unknown，带包终态提交仍拒绝，公开带包请求在登记前拒绝。

双目标 host ASan/UBSan 回归覆盖有效 REUSE、空来源／有来源 WRITE 的字段读回、无包 worker 遇带包收据不擦槽，以及带包收据在 pending／原槽启动时不确认、不回收。固定 SDK 双目标签名构建与官方验签见本轮验证记录；这些证据只证明只读恢复输入和显式执行门，未覆盖包状态机的跨提交恢复、业务试运行或实体板，P6-10/P7-04 不验收。

## 2026-09-30 联合 OTA 公开参数与客户端合同

`ota.start` 现要求 `package_mode`，无包只允许原固件参数；`reuse` 还须提供包 SHA-256、长度、guest ABI、data schema 与随后代表事件的原始字节 SHA-256；`write` 另须提供包 HTTPS URL。解码使用各模式精确键集合，完整值进入本 boot 请求指纹。公开串口客户端按本地签名文件计算固件与包的摘要／长度，C3 包槽上限为 `0x77000`、ESP32 为 `0x82000`，`reuse` 还核对设备当前确认包摘要。`ota.result` 现带包模式及可空包摘要，客户端严格核验字段。

此处仅冻结设备与公开客户端的参数合同：设备对带包 `ota.start` 在登记 V3 收据、停止 guest 或任何 Flash 擦写前返回 `failed/product_ota_unavailable`，非法 `write` URL 更早返回 `invalid_request`。无包路径仍运行；带包 HTTPS 下载、启动恢复、授权事件后 30 秒联合健康确认及实板验证尚未完成，P6-10/P7-04 不验收。C3／ESP32 host ASan/UBSan 全套、公开客户端 Python 15 项、固定 SDK 双目标测试键签名构建及官方 RSA v2／ECDSA v1 验签通过；签名 app 分别为 `0x121000/0x130000`、`0xffff4/0x120000`，未写实体板。

## 2026-09-30 联合 OTA 内部 WRITE 包槽续写

Base 新增仅供同一 OTA owner 使用的 `write_staged_firmware_package`。它在擦包槽前从原 V3 收据重新核对 operation ID、A/C 签名固件摘要、目标镜像大小、来源包身份和旧 B 退役后精确的 `WRITING` 序号；来源有包时还要求 guest 已停止、join 并回收原生资源。锁定 Container 将来源字节写入已预约的非来源包槽，回读完整 SHA-256，验证包签名、Wasm 与授权，持久提交并独立读回 `PREPARED`。参数或持久状态冲突在擦写前拒绝；一旦开始写入，断流、摘要错误或读回不确定均保持未决，不选择新 app boot。

C3／ESP32 host ASan/UBSan 与锁定 Container/WAMR 的双目标真实签名 guest 生命周期各 100 次重装通过。新增用例验证从空来源和已确认包来源续写、错误 operation 无擦写、真实签名包 `PREPARED` 读回、来源槽保持原字节、重复续写拒绝，以及篡改下载字节时保留 `WRITING` 和旧包。固定 SDK 双目标签名构建及官方 RSA v2／ECDSA v1 验签通过，签名 app 仍分别为 `0x121000/0x130000`、`0xffff4/0x120000`。测试调用内部入口并使用构造 V3 收据；公开带包 `ota.start`、HTTPS 来源、启动恢复、代表事件后 30 秒联合确认、掉电和实体板仍未完成，P6-10/P7-04 不验收。

## 2026-09-30 联合 OTA 内部包槽 stage

Base `stage_firmware` 现由原 V3 收据提供 operation ID、A/C 镜像摘要、来源／目标包身份及原 ECS2 sequence。旧 B 退役后，它要求 ECS2 只剩唯一 A 绑定且序号精确前进；来源有包时须先停止 guest、join 并证明原生资源回收。`REUSE` 用锁定 Container 的验证回调从来源槽重新读取签名包、Wasm 与授权，合格后持久提交 `PREPARED`；`WRITE` 只预约不占来源包的目标槽并提交 `WRITING`，不下载或擦写包字节。`WRITE` 的返回状态不能触发 `eota_select`，须待独立包写入、验签与 `PREPARED` 读回。

C3／ESP32 host ASan/UBSan、锁定 Container/WAMR 的双目标签名 guest 生命周期各 100 次重装及新增 stage 回归通过。回归用构造的 V3 收据验证无包原路径、从空绑定 `WRITE`、带包来源 `REUSE`／`WRITE`，以及旧包字节损坏、过期序号、未停止 guest 和重复调用均不产生错误持久提交；成功 stage 各仅提交一次 ECS2，包 Flash 擦写计数不变。固定 SDK 双目标测试键签名构建与官方 RSA v2／ECDSA v1 验签通过，签名 app 为 `0x121000/0x130000` 与 `0xffff4/0x120000`。公开 `ota.start` 和启动恢复仍只允许无包；包写入、联合健康确认、掉电与实体板尚未完成，P6-10/P7-04 不验收。

## 2026-09-29 带包 OTA V3 内部登记校验

内部 `esp_base_ota_receipt_register` 现接受经同一存储 claim 取得的 `REUSE`／`WRITE` 来源快照，并在提交 V3 前核对目标包摘要、长度、ABI、data schema、代表事件摘要和来源包身份。`REUSE` 必须逐项等于已确认来源包；`WRITE` 可从空来源开始，来源有包时要求相同 data schema。同 operation ID 的固件或包参数变化返回冲突，不再把只相同的固件摘要视为同一请求。V3 解码也拒绝超出有符号读取上限的包长度和跨 schema 的 `WRITE` 收据。

C3／ESP32 两目标 host ASan/UBSan 回归通过，覆盖有效带包登记／读回、同 ID 冲突、错误来源、跨 schema、超出有符号读取上限和缺失事件摘要。固定 SDK 双目标测试键签名产品构建及官方验签通过，C3 RSA v2 镜像 `0x121000/0x130000`、ESP32 ECDSA v1 镜像 `0xffff4/0x120000`。公开 `ota.start` 仍只解析无包请求，带包收据的启动恢复继续返回存储不确定，不能凭内部登记调用擦写目标槽；本检查点未写实体板。P6-10/P7-04 保持进行中。

## 2026-09-29 带包 OTA 目标容量与身份预检

只读来源快照现在接收完整 OTA 请求。在原升级存储 claim 下，`REUSE` 逐项核对目标包 SHA-256、长度、ABI、data schema 与已验签来源包；`WRITE` 在保留当前包槽后按 Container 的槽选择条件预检是否还有足够大的目标槽，来源有包时拒绝跨 data schema。带包模式还要求非零目标包摘要、长度、ABI、schema 和请求绑定的代表事件摘要；无包模式拒绝带入这些字段。预检失败清空输出，不登记 V3 收据、退役旧 B 或擦写 app／包槽；公开 `ota.start` 仍仅解析无包合同。

C3／ESP32 host ASan/UBSan 测试覆盖错误包身份、长度、schema、缺少事件摘要、仅来源槽可容纳目标及原有序号边界；锁定 `esp-container@e8a0d0b`、WAMR `c10736f` 的双目标签名 guest 生命周期各完成 100 次重装，核对快照没有 NVS blob 或包 Flash 写入。固定 SDK 双目标签名应用构建和官方验签通过：C3 RSA v2 `0x121000/0x130000`、SHA-256 `6b8a1807e20ebfcc7ee9bbb60addb3280b773386ccba75684075a3be7001fbf0`；ESP32 ECDSA v1 `0xffff4/0x120000`、SHA-256 `5b7898f6438f55b99e445040f0875bac829786c3bd7d2b8ce4ee37637f58781e`。这是软件预检，不是带包 `ota.start`、联合升级或实体板验收；P6-10/P7-04 继续进行中。

## 2026-09-29 带包 OTA 来源身份只读快照

在原升级存储 claim 下，Base 先通过双次签名固件观察取得运行 A 与可选 B 的摘要，再以 Container `reconcile` 核对 ECS2 确认态、固件绑定和包引用。`REUSE` 仅接受运行中、事件入口开放的已确认包；`WRITE` 可接受同样的来源包或 `EMPTY` 无包来源。快照复制来源包 SHA-256、长度、ABI、schema 和 ECS2 序号，不登记 OTA 收据、不擦写包槽或 app 槽。对 `WRITE` 额外预留一次 `WRITING` 持久序号，双固件来源共需 7 次、单固件来源共需 6 次序号余量；`REUSE` 分别为 6 次和 5 次。无包原路径维持其原有条件。

C3／ESP32 两目标 host ASan/UBSan 回归通过，假件验证模式拒绝、来源元数据、空来源和序号边界；锁定 `esp-container@e8a0d0b` 与 WAMR `c10736f` 的真实签名 guest 生命周期各通过 100 次重装，并证实两种快照无 NVS blob 或包 Flash 写入。固定 SDK 双目标签名产品构建及官方验签通过：C3 RSA v2 `0x121000/0x130000`，ESP32 ECDSA v1 `0xffff4/0x120000`。这些都是软件与合成存储证据；公开 `ota.start` 仍只接收 `NO_PACKAGE`，带包 V3 收据恢复保持阻断。REUSE／WRITE 的授权、写入、跨提交恢复、代表事件联合健康确认及实体板验证仍未完成，P6-10/P7-04 不验收。

## 2026-09-29 OTA V3 联合意图存储前置

无包 OTA 的最新收据从 186 字节 V2 硬切为 308 字节 V3，新增包模式、目标包长度／摘要／ABI／schema、代表事件摘要及来源包身份字段；提交后仍逐字节读回。当前正式 `ota.start` 只登记 `NO_PACKAGE`；带包请求在登记前拒绝，读到带包 V3 或旧 V2 长度时启动恢复保留不确定状态，不能凭字节完整性执行擦除。双目标 Base host 回归、固定 SDK C3 正式产品测试键签名构建与 RSA v2 验签、ESP32 测试键签名构建与 ECDSA v1 验签通过。C3 签名 app `0x121000/0x130000`，ESP32 `0xffff4/0x120000`；两者尚未运行联合 OTA。

新 V3 长度的 11 页 C3 和六页 ESP32 合成 NVS 均在三次独立 QEMU 启动完成 100 轮最大配置、OTA 收据形态、ECS2 和八条产品账本写入／读回，最终可用 entry 分别为 `960/1386` 与 `328/756`，官方非空页 CRC 通过；完整输入摘要见[C3 十一页](c3-eleven-page-nvs-capacity.md)与[ESP32 六页](esp32-six-page-nvs-capacity.md)容量记录。V3 源码只是 P6-10 的持久意图前置：公开带包授权、REUSE／WRITE 准备、代表事件完成后的联合确认与跨提交恢复仍未接通；未写实体设备，P6-10/P7-04 保持进行中。

## 2026-09-29 双板新鲜恢复基线与迁移阻断复核

按五仓计划第 13.1 节，在 `mac-pro-1` 重新核对两块 4 MiB 板的串口、芯片、MAC 与原固件响应。C3 当前仍为 Base v1、配置 revision 5；ESP32-D0WD-V3 仍为 ESP-AT 1.1.b1.0。esptool 与 espefuse 只读探测会复位目标：C3 观察到 boot ID 变化后重新确认同一业务 UUID 的哈希和 revision，ESP32 最终硬复位后 `AT+GMR` 返回原版本及 OK。两块板的 Secure Boot 和 Flash Encryption eFuse 均未启用；本轮没有写 eFuse、Flash 或分区。

各板分别取得两份 4 MiB、权限 `0600`、逐字节一致的完整 Flash 备份。C3 两次独立读取相等；ESP32 每次复位后读取的默认 NVS 相差 117 字节，因此让旧 AT 暂停在 bootloader 中连续读取两次，取得全片一致的配对恢复件，随后恢复旧 AT。固定 SDK 分区工具核对 ESP32 旧布局；当前 Base v3 离线预检用新的 C3 配对备份复跑，仍因 `base_store` 后续页无法安全审计而阻断，未生成 v3 候选。原始镜像、设备身份、安全读取、日志及尚未执行的同板恢复步骤仅在 ESP Tool 忽略的私有 `p6-04-board-baseline-20260929/` 保存。备份一致性不代表恢复写回演练、签名产品基线或 P7-01 迁移通过；C3 异常页与 ESP32 旧 AT 分区仍需分别解决。

## 2026-09-29 产品代表事件确认窗口

维护者确定安装／升级请求必须绑定随后业务事件原始 guest 字节的 SHA-256；Base 精确解码并纳入原操作指纹，公开串口客户端从 `--trial-event-file` 计算同一值。候选 guest 完成同包、同摘要的授权事件且返回非负业务结果后，Base 连续 30 秒核对 Wi-Fi、可信时间、MQTT ready、代表事件、失败计数与最多 1 秒的控制循环间隙。执行器在事件锁下保留代表事件完成序号并累计 guest/runtime 失败；后续普通成功事件不会覆盖它，试运行期间发生的失败也不能被下一条成功事件掩盖。定时器或宿主日志读取失败先关闭事件入口并撤销代表事件。离线、错事件或 guest/runtime 失败不会确认；窗口通过后重新核对 Container 代表事件、失败计数与队列空闲。若短暂处理定时器导致提交尚未开始，释放本次短时 claim 并重试；持久提交开始后才把不能证明的结果留作 unknown。确认成功持原产品存储 claim 完成 `HEALTH_VERIFIED`、`CONFIRMED` 的独立读回，将原 operation ID 账本写为成功；不确定时保留 claim，由下次启动对账，不自动重放请求。

C3／ESP32 主机 ASan/UBSan 测试覆盖错事件、离线重置 30 秒窗口、失败后重新计时、提交前短暂忙重试、最终成功、确认不确定保留 claim 与请求指纹冲突；10 项串口伪设备测试通过。锁定 Container/WAMR 的双目标真实签名 guest 生命周期（含各 100 次重装）通过，新增代表事件后另一条成功事件仍能按原代表事件确认的回归。固定 SDK 双目标签名 app 完整构建及官方验签通过：C3 RSA v2 `0x111000/0x118000`，ESP32 ECDSA v1 `0xffff4/0x120000`。这轮没有生产 Broker 消息或两块实体板产品事件，也没有五能力容量与掉电恢复验收；P6-04/P7 仍按完整门槛推进。

## 2026-09-29 Base 启动与配置 NVS 仲裁

正式 Base 启动的默认 NVS 初始化、设备身份读取和 v3 配置读取现各自持有与 FRP scratch／OTA／Container 相同的短时 Flash I/O owner；配置连接证明后的提交和读回也持有该 owner。控制任务发现 owner 忙时保留候选及原截止时间，在下一轮重试，不分配配置工作区或写 NVS；提交后的 owner 释放失败报告 `storage_uncertain`。C3／ESP32 主机 ASan/UBSan 回归覆盖 NVS／身份访问持有、配置忙时不读取、候选延期提交和正常释放。固定 SDK 双目标签名应用构建与官方验签通过：测试键 RSA v2 C3 镜像 `0x111000`，对 `0x118000` 槽余 `0x7000`；ECDSA v1 ESP32 镜像 `0xffff4`，对 `0x120000` 槽余 `0x2000c`。本检查点不代表真实 Flash 时延、磨损或五能力实板并发通过。

## 2026-09-29 Container Flash 租约释放失败闭合

Base 将短时 Flash I/O owner 的释放结果交给公开 `esp-container@e8a0d0b6384bbba813b955ed08ebc315c134a707`，C3／ESP32 主应用和 NVS 容量探针各自的 Component Manager 锁均更新到这一精确版本。Container 的 NVS 与包 Flash 读写在释放失败时报告 I/O 失败；映射解除失败时关闭刚装载的 WAMR runtime，不进入 guest。Container 真实 WAMR 主机 CTest 9/9、Base 双目标 ASan/UBSan 主机测试与真实签名 guest 生命周期（各 100 次停止／卸载／重装）通过；固定 SDK 下两目标签名主应用和 NVS 探针均完整构建。测试键 RSA v2 C3 镜像 `0x111000`，低于 `0x118000` 槽 `0x7000`；ECDSA v1 ESP32 镜像 `0xffff4`，低于 `0x120000` 槽 `0x2000c`，官方验签均通过。本轮没有写设备；最长映射占用、真实 Flash 故障和五能力同机并发未验收。

2026-09-29 P4-05/P6-03 OTA 槽状态读取仲裁：公开 OTA `04acb5e80a744649f8442607fb8d901d30880ca0` 经 Base 唯一清单与双目标锁解析，组件摘要均为 `865f054e54efba8c0f153387d1902f550a5f4342a8083646ae567916559eec3f`。OTA 库的运行／boot／目标槽、otadata 状态、回退资格和故障读回逐次持有共同 Flash I/O claim；Base 启动和确认后的直接 `eota_inspect` 也持有该 claim。双目标 host ASan/UBSan、固定 SDK C3 RSA v2／ESP32 ECDSA v1 仓外测试键签名构建与官方验签通过，签名 app 分别 `0x111000`／`0xffff4` B。SDK 整镜像验签及回退资格检查的最长占用、FRP 最大合法记录同机期限和实体 Flash 寿命仍需实测；本轮未写实体设备，P4-05/P6-03/P7 不据此验收。

2026-09-29 P4-05/P6-03 OTA 显式读取接线：公开 OTA `a6bf4e362756ea2cee9febc95555a6866af4c931` 经 Base 唯一清单与 C3／ESP32 两目标锁解析，组件摘要均为 `7faec2419173ce4c935efa1159bfe0047344e0f5e79567c73030d17a791f7ddf`。OTA 库的显式分区读取每次最多 1024 字节取得共同 Flash I/O claim，释放后计算摘要；SDK 整镜像验签在整次调用期间持有 claim。Base 双目标 host ASan/UBSan 通过，固定 SDK 仓外测试键签名 C3 RSA v2／ESP32 ECDSA v1 构建及官方验签通过，签名 app 分别 `0x111000`／`0xffff4` B。槽状态观察和验签调用的物理时延、最大 FRP 记录与正式 OTA 同机进展及两板寿命预算仍未测量；本轮未写实体设备，P4-05/P6-03/P7 不据此验收。

2026-09-29 P4-05/P6-03 短 Flash I/O 软件接线：公开 OTA `195201aed3f7c5ddd13517b7d8ad3dc2877e4ab4` 经 Base 唯一清单与 C3／ESP32 两目标锁解析，组件摘要均为 `9ba1e3566247cf0ecb2db290c568fe1b9841ef7abd240e947e31356341ebbea9`。Base 启动绑定同一短 owner，OTA 库对旧槽退役、逐扇区准备、切槽和失败恢复中的 app／otadata 写调用逐次获取／释放；Base 的 pending 确认／回滚与收据 NVS 读写也接入，FRP scratch 和 OTA 各次获取最多等待 500 毫秒，网络和进度回调不持锁。双目标 host ASan/UBSan 通过，含 NVS 假件持锁断言、确认期间 scratch 争用超时及释放后可再获取。固定 SDK 普通双目标构建和仓外测试键签名双目标构建通过，C3 RSA v2／ESP32 ECDSA v1 官方验签通过，签名 app 分别 `0x111000`／`0xffff4` B。本轮未进行实体设备写入、最大 FRP 记录与 OTA 同机推进或大范围 Flash 读验签时延／寿命测量；P4-05/P6-03/P7 不据此验收。

2026-09-29 P6-03 OTA Flash 写入切片：Base 的唯一 OTA 清单和 C3／ESP32 目标锁已精确更新到公开 `esp-ota@a00f0a76959ed7e3ef4b999dab1e54b9a87f469b`，组件摘要均为 `5acb530d88b692f495324caaa9d008299ea15bca476e8c1cbccac2339376dd55`，两锁除 target 外一致。上游将 `esp_ota_begin` 改为固定 SDK 支持的 `OTA_WITH_SEQUENTIAL_WRITES`，让下载过程中由 SDK 随 `esp_ota_write` 逐扇区擦写，避免开始下载时一次擦除整张应用镜像。Base 两目标 host ASan/UBSan 与固定 SDK 普通构建通过，C3／ESP32 镜像分别为 1,036,736／974,480 B。本项仅证明源码消费与离线编译；OTA 应用、otadata 和收据 NVS 写入仍须统一接入短 Flash I/O 仲裁，并进行同机负载与实体板验证，P6-03/P7 不据此验收。

2026-09-29 P6-04 业务事件客户端断线语义：公开发布器原来使用 Paho 2.1.0 的默认自动重连；该库在干净会话重连时可能再次发布尚未确认的 QoS 1 消息（[Paho 官方说明](https://github.com/eclipse-paho/paho.mqtt.python#known-limitations)；[固定 2.1.0 源码](https://github.com/eclipse-paho/paho.mqtt.python/blob/v2.1.0/src/paho/mqtt/client.py)），与本客户端“只发布一次、状态不明按原事件核对”的合同冲突。现在显式关闭自动重连，发布后断线仍只返回 unknown，不创建新序号或重发。发布器伪 MQTT 回归新增断线后仅一次 `publish` 和禁用自动重连参数断言，6/6 通过；原帧生成测试 1/1、Python 编译与 `git diff --check` 通过；临时安装固定 Paho 2.1.0 并实例化真实客户端，确认默认 `reconnect_on_failure=True`、本客户端传入后为 `False`。本轮没有真实 Broker、设备或试运行确认结果，P6-04/P7 不据此验收。

2026-09-28 P6-03/P6-04 C3 对照：保留正式 USB 控制台与三 `0x82000` 包槽的同源码测试键 RSA v2 签名 app `0x121000` B，官方尺寸门拒绝双 `0x120000` app 槽，各超 `0x1000` B。仓外仅为 QEMU 改用 UART、双 `0x130000` app 与三 `0x74000` 诊断包槽后，正式 JSON `product.uninstall`、同片冷启动持久结果及同 ID 不重执行均通过，C3 6 KiB 控制栈未溢出；官方 NVS CRC、账本/ECS2 CRC 与 Flash 分区不变性读回通过。关部分曲线、错误名查表和 NIST 优化的组合可暂时降到 `0x111000` B，但内容距下次签名台阶仅 56 B，且未验证真实 TLS/时延，不作为产品配置或完整布局验收。无实板写入；[精确输入与容量阻断](product-uninstall-c3-protocol-qemu-checkpoint.md)。

2026-09-28 P6-04 串口产品卸载签名 QEMU：原 ESP32 控制任务 4,096 B 在 `product.status`、6,144 B 在 `product.uninstall` 触发真实 FreeRTOS 栈溢出；目标独立调整为 ESP32 8,192 B、C3 仍 6,144 B。最终源码重新签名并按新 app 摘要制造 ECS2 sequence 6、空 EPRD 及 4 MiB Flash，正式串口命令完成卸载，冷启动后原操作结果和相同 ID 的重复请求均保持 sequence 7／账本高水位 1；官方 NVS CRC、账本/ECS2 CRC 与逐区 Flash 读回通过。C3／ESP32 普通构建与 host ASan/UBSan 均通过。无 Broker、HTTPS、FRPS 并发或实体板，P6-03/P6-04/P7 不勾验收；[精确输入与日志](product-uninstall-protocol-qemu-checkpoint.md)。

2026-09-28 P6-04 公开卸载写入口：`a99c02c` 将 `product.uninstall` 接入设备命令、持久 EPRD 意图／终态和正式 Container 停止／卸载；复位后只读核对 ECS2 原 UUID、序号及包绑定，能证明提交或未提交才写回终态，无法证明时阻断 READY。`9da7aba` 将串口设备回执超时接回原 operation ID 的只读结果查询，写命令不重发。C3／ESP32 宿主 ASan/UBSan、8 项伪串口测试、固定 SDK 两目标普通完整构建及真实签名 guest 的卸载恢复生命周期通过；本地 app 分别为 `0xe3eb0`／`0xd5c10` B。没有写实板，真实掉电、Broker 设备联调、公开安装／升级、包来源及试运行健康确认尚未验收，P6-04/P7 总状态不变。

2026-09-28 P6-04 guest 事件结果观察软件续进：Base 将已授权 MQTT 事件的本 boot 序号随有界队列副本交给唯一产品线程，线程在真实 `on_event` 返回后记录最近一次包 SHA-256、完成序号、runtime 是否成功和原始 guest 整数结果；负数被报告为业务失败，runtime 异常不伪造 guest 返回值。每 5 秒的非 retained `reported` 同时给出入队高水位与最近完成观察，签名包换包启动清空旧观察；这些易失字段仍非持久产品操作结果，也不能单独证明试运行健康。C3／ESP32 全套 host ASan/UBSan、固定 SDK 普通完整构建通过，app 分别 `0xe3060`／`0xd4ff0` B；两目标以公开 Container `d370899b`／WAMR `c10736f` 跑真实签名 guest 的生命周期、同 boot 换包和各 100 次卸载重装，C3／ESP32 均通过。宿主假 Flash/NVS、无生产 Broker 与实板业务事件，P6-04/P7 验收状态不变；没有写设备。

2026-09-28 P6-04 独立 MQTT 业务事件软件接线：普通 Base 在同一严格 TLS 会话订阅精确 `command`、`event`，两个 SUBACK 均批准后才 ready。`event` 的域隔离 HMAC 将当前 boot、设备 UUID、完整包摘要、连续 64 位序号和 guest 原始字节绑定；只有正式产品有界 FIFO 接收后才推进同 boot 高水位，并在非 retained `reported` 报告。QoS 1 PUBACK、已入队及 guest 业务成功是三个不同事实。公开 `tools/product_event.py` 的宿主生成帧与固件固定向量一致；C3／ESP32 全套 host ASan/UBSan、两目标固定 SDK 普通完整构建和 Python 向量测试通过，app 尺寸分别 `0xe2df0`／`0xd4d30` B。`mqtt-service` 源码渲染器已为登记设备增加精确 `event` ACL，但当前生产 Profile 没有 Base 账户，Broker 镜像/真实 ACL、设备消息、guest 产品结果和试运行确认均未验收；没有刷写设备，P6-04/P7 状态不变。

2026-09-28 P6-04 业务事件执行端切片：Base 与独立 NVS 探针的 C3／ESP32 四份锁均从新清单解析至公开 `esp-container@d370899b88883d8c23c60884dda9e2dae8bc295d`。Container 在成功验签并打开选定包后返回签名 `event_queue_limit` 和包 SHA-256；Base 只在 `init` 后按该上限分配 FIFO，入队校验事件长度、当前运行态与完整包摘要，唯一产品 pthread 串行调用 `on_event`，停止时释放未交付副本。同 boot P1→P2 更换签名包后旧摘要拒绝，C3／ESP32 两种宿主目标的 ASan/UBSan 真实 guest 生命周期与各 100 次回收、macOS 第 10／50／100 次已用内存稳定检查通过；Base C3／ESP32 固定 SDK 普通完整构建、C3 全套 host 通过。当前事件仅由宿主用例投递，设备 MQTT 独立业务 Topic、Broker ACL、消息授权、guest 结果与试运行确认仍未实现；`ACCEPTED` 和 runtime 完成计数均不得视作业务健康，P6-04/P7 状态不变。没有写入设备。

2026-09-28 P6-04 只读序号查询切片：正式 USB／已认证 MQTT 协议增加 `product.status`，返回持久账本高水位、下一序号和未决原操作 ID；NVS 键缺失返回 `unknown/product_ledger_uninitialized`，不在查询中建账，下一序号仍须在未来写入时由账本原子核对。公开串口客户端检查响应设备／boot、字段和序号关系。C3／ESP32 host ASan/UBSan 套件、串口伪设备 7 项及固定 SDK 双目标普通／显式离线全量构建通过；C3 app `0xe25b0` B，ESP32 app `0xd45e0` B。没有公开产品写命令、真实业务事件或设备刷写，本切片不改变 P6-04/P7 验收状态。

2026-09-28 P6-04 产品操作持久结果软件切片：按维护者“最近固定条数”裁决，Base 新增 EPRD v1 单 blob 账本候选，暂存最近 8 条及不回退的 `operation_sequence`；缺失 NVS 键为未初始化，不自行重置序号。读写经过共享短时 Flash I/O owner，意图与终态各自 commit 后逐字节读回；未决意图跨 boot 阻止下一操作。只读 `product.result` 和公开串口客户端已按原 ID 查询，产品写入口保持未开放。Base 主固件与独立 NVS 探针的 C3／ESP32 四份 Component Manager 锁均从空重新解析并精确消费已公开的 `esp-container@f82e4b8f57eb6ae75309d5cfb7472feef2380912`，其 IDF 包槽/NVS provider 也使用同一短时 I/O owner。Container host CTest **6/6**、Base 双目标完整 ASan/UBSan host 套件、公开设备客户端六项测试、固定 SDK 普通 C3 和显式未签名离线 ESP32 主固件构建通过；新镜像都不是实板刷写候选。

同日按真实八槽布局在 C3 六／八页、ESP32 六页三档独立合成 4 MiB QEMU Flash 各跑 100 代最大 Base 配置、OTA V2 合成形态、Container ECS2 和 910 字节产品账本；每代账本双提交、每次回读，三档均通过阶段间重启与官方 NVS parser。最终六页 `used/free/available/total=297/459/333/756`，八页为 `298/710/584/1008`；最高页序号分别为 299、260、299。输入、日志摘要和边界见[C3](c3-eight-page-nvs-capacity.md)及[ESP32](esp32-six-page-nvs-capacity.md)容量记录。该结果仅是固定 SDK／QEMU 正常写入容量，未证明首次建账所需持久 Container 基线、产品写命令、真实 Flash 磨损、掉电或实体五能力，P6-03/P6-04/P7 不勾验收。

2026-09-28 P6-03 MQTT IRAM 精确锁：Base 双目标正式锁定 `esp-mqtt@bebde3971c2f4b4ee99e150348213222bfd9e27e`。仅经典 ESP32 单核且启用 8BIT IRAM 时，MQTT 常驻实例与按消息存活期分配的入站消息体放入该区域；C3 仍按原路径分配。固定 SDK 双目标 host ASan／UBSan、正式 CSV 签名产品构建、尺寸和官方验签通过。仓外签名 ESP32 guest／隔离 MQTT TLS Broker／官方 FRPS 单工作流及并发一条 4096 B 入站消息的普通 8BIT 堆历史最低 **53,380 B**；同镜像三条各 4096 B 入站消息复测为 **53,348 B**，均高于不变门。第四条在途、双流、OTA、正式 owner 和实体板仍未测，[精确输入与边界](p6-03-mqtt-iram-precise-lock-checkpoint.md)单独记账，P6-03/P7-02 继续开放。

2026-09-28 P6-03 MQTT 入站消息存活期精确消费：Base 双目标锁定公开 `esp-mqtt@a67cb8fd4f146550c210487dc281248967b6c61a`，空闲 MQTT 实例中三份固定消息体改为按入站消息实际存活期分配，字段静态差额 **13,092 B**，不计作五能力堆低水增量。固定 SDK 的双目标 host ASan／UBSan、正式 CSV 签名产品构建、尺寸与官方验签通过；ESP32 从空配置生成的单核／8BIT IRAM 默认项修正也通过独立离线构建。随后新锁签名 ESP32 QEMU 在 Container guest 运行、空闲 MQTT client 实例占用和官方 FRPS 单工作流同存时完成严格 TLS 与 300001 B 双向回显，历史最低普通 8BIT 堆 **65,780 B**；MQTT 未连 Broker、OTA 未运行，仍不作五能力验收。[双目标输入、QEMU 和未验收边界](p6-03-mqtt-message-lifetime-precise-lock.md)。

2026-09-28 P6-03 Base 控制工作区存活期收敛：串口行、配置候选、命令解析和 MQTT 修订装配区退出常驻 `.bss`；新修订装配内存不足时撤销旧 MQTT 端点及管理密钥。固定 SDK 的 C3／ESP32 host ASan/UBSan 全套通过；当前锁的 C3 仓外签名 guest、严格 TLS／官方 FRPS 和双向各 300001 B 工作流再次通过，普通内部 8BIT 堆历史低水 **57,020 B**，比 49,152 B 门高 7,868 B。该诊断暂用缩小包槽的几何，且没有 MQTT Broker／OTA HTTPS 同机负载；[逐轮容量检查点](p6-03-c3-current-lock-frps-work-qemu-checkpoint.md#base-控制工作区存活期收敛)记录前后读数和未测边界。

同日另在仓外验证严格对齐的 C3 **4 MiB** 软件分区候选：双 `0x118000` app 起点 `0x20000`／`0x140000`，三份 `0x82000` 包槽、独立 `0x10000` FRP scratch 和 18 页 Base NVS 同时装入。首次紧贴排列因 `ota_1@0x138000` 未按 64 KiB 对齐被官方工具拒绝；修正后当前源码 RSA v2 签名 app `0x111000`、官方验签及尺寸门通过，合成 Flash 的真签名 guest 返回 `RUNNING`、Base 到 `READY`，双 app／包区／NVS 读回未变。旧 C3 存储重叠迁移、五能力同存和实板仍未验收；[候选几何与原始证据](p6-03-c3-aligned-layout-software-probe.md)单独记账。

2026-09-28 P6-03 C3 静态 Wi-Fi RX 容量续验：保持 AMPDU RX BA 窗口 6，把常驻 RX 缓冲 10 → 6；同一当前锁签名 guest／官方 FRPS 严格验签、注册、Pong 和双向各 300001 B 回显再次通过，普通内部 8BIT 堆历史最低 **29,856 B**，比同输入 10 缓冲组高 **5,980 B**，但距 **49,152 B** 门仍差 **19,296 B**。新默认项和实际生成值纳入 CMake 检查；C3 正式分区 RSA v2 产品重签为 `0x111000` B 并经官方验签、尺寸门。诊断使用 OpenETH，不证明实体 Wi-Fi 吞吐或完整五能力并发；[单变量输入、原始 GDB 与读回](p6-03-c3-current-lock-frps-work-qemu-checkpoint.md#c3-六个静态-rx-缓冲续验)保留，P6-03/P7-02 未验收。

2026-09-28 P6-03 产品默认内存项落盘：Base 两目标共同启用 SDK TLS 动态收发缓冲，C3 关闭两项 Wi-Fi IRAM 优化；保持完整 TLS 片段、证书校验和 49,152 B 堆门。仓外复制当前源码／锁并用测试键重建正式分区的双目标签名产品：C3 RSA v2 app `0x111000` B、ESP32 ECDSA v1 app `0x10fff4` B，官方签名、分区与 app 尺寸检查和双目标 host ASan/UBSan 通过。正式 CSV 仍无 FRP scratch，本轮没有烧录设备，也没有把上一轮 C3 FRPS 工作流的最低 23,876 B 当成通过容量门；[完整输入与边界](p6-03-c3-current-lock-frps-work-qemu-checkpoint.md#产品默认项与双目标签名构建)。

2026-09-28 P6-03 C3 当前锁签名 guest／FRPS 工作流容量：仓外仅关闭两项 Wi-Fi IRAM 开关的诊断，在正式 Container `RUNNING`、Base `READY` 后完成官方 FRPS 严格 TLS／注册／Pong 和一条双向各 **300001 B** 的真实工作代理回显，`work.completed=1`、`work.failed=0`。两次 QEMU 历史最低普通内部 8BIT 堆为 **6,460／7,072 B**；含 guest 返回断点的第二次比 **49,152 B** 门低 **42,080 B**。诊断签名 app `0x121000` 超出保留三份最大产品包的既有 `0x118000` app 候选槽，只能在仓外缩小包槽的几何中验签运行。默认 IRAM 配置的同锁诊断到 `AUTHENTICATING`、`verify=0`，未注册。该次实验时正式配置与分区未变，FRP 仍绕过 Base owner，MQTT／OTA 和实板未测；[原始输入、签名、GDB 与读回](p6-03-c3-current-lock-frps-work-qemu-checkpoint.md)不构成 P6-03／P7-02 验收。

同一关闭 Wi-Fi IRAM 的输入仅在仓外启用固定 SDK TLS 动态收发缓冲，最大入站／出站 TLS 片段仍为 16 KiB／4 KiB。新签名 guest 与官方 FRPS 登录、注册、Pong、同一 **300001 B** 双向工作流再次通过，普通内部堆历史最低升至 **23,876 B**，比 49,152 B 门仍低 **25,276 B**。签名、配置单变量、GDB 和 Flash 读回补入上述检查点；该实验时点动态缓冲尚未进入产品配置，断线重连与实板未测。

2026-09-28 P6-03 Base 配置提交单缓冲：v3 NVS 提交前编码、写入后读回与配置指纹在唯一控制 owner 中复用同一 7,618 B 静态缓冲；读回仍经严格解码，并逐字段核对候选和下一 revision，任何不同保持不确定结果。固定 SDK 双目标 ELF 各减少 **7,618 B** 常驻 `.bss`，主固件普通构建及 host ASan/UBSan **20/20／19/19** 通过，包含 Wi-Fi、MQTT、FRP 有效但被改写的持久读回拒绝。同源 ESP32 仓外 IRAM／官方 FRPS 签名 QEMU 两次运行的普通堆历史最低为 **40,508／38,968 B**，低于 **49,152 B** 门；与旧单次 **32,100 B** 对照有运行时波动，不把静态释放机械等同堆增益。输入、镜像摘要与未测边界见[配置单缓冲检查点](p6-03-config-single-buffer-checkpoint.md)，P6-03 仍未验收。

2026-09-28 FRP 严格 TLS 成功状态精确消费：公开 `esp-frp@b462c1497438cfeb514f022bc2d2d6c026599a41` 在握手成功后把 TLS 验签标志复制到 client 就绪状态，修正先前 `verify=UINT_MAX` 的状态快照；FRP 的官方 FRPS `client_contract`／`client_upstream` 两项宿主测试通过，包含初次连接和重连的 `verify_flags==0` 断言。Base 主固件清单与固定 ESP-IDF `578cf89c` 的 Component Manager **全新解析**出的 C3／ESP32 锁精确消费该提交，FRP 组件摘要均为 `f0ce6da3d1db2598e28798a2e97c8ad89d91e53cc18b0770c45e353d17d65a32`；两锁 SHA-256 分别为 `1ef8526ea6e8238d58c5c26bc25f7a23534524b9f8a5df96cb6132596cd87ac3`／`9685b02e3e7c1c730e2a3d89daa7e3e0a84d5f8f3213323ac5de3c18a23eb856`，除 FRP 提交、组件摘要及清单摘要外其余依赖不变。NVS 容量探针不引用 FRP，其两份锁保持原样。

隔离 `mac-work-1:/private/tmp/esp-base-frp-status-consumer-20260928/` 完成 C3 与 ESP32 普通完整构建和 Base 双目标 host ASan/UBSan 回归。C3 未签名 app 为 `0xdefd0` B、SHA-256 `a8acb0289526449808d7a0d2cdade8916e884cde12e070e3ecfa629a996d65a3`；ESP32 显式离线探针未签名 app 为 `0xd2bc0` B、SHA-256 `736d90b47d5faebb1fdd269219151721c4d65c62d56ef95ec961eb0a0c34b8fc`，两目标官方 app 分区尺寸检查均通过。这一轮只修正状态上报并闭合新依赖，没有改变 TLS 严格校验、P6-03 堆水位或实体板验收结论；镜像不是刷写候选。

2026-09-28 P6-03 ESP32 IRAM 容量隔离实验：在仓外签名 QEMU 诊断中使用固定 SDK 的单核／可字节访问 IRAM 与 TLS 大缓冲分配后，同一 64 KiB ABI 2 guest 下的官方 FRPS 登录、代理注册和 Pong 首次走通，普通 8BIT 堆历史最低空闲为 **17,248 B**。再将 FRP 会话本体约 14.9 KiB 严格放入 IRAM，重签、重生成 ECS2／Flash 并独立运行后仍成功，最低空闲提高到 **32,100 B**，低于 48 KiB 门 **17,052 B**。输入 SHA、签名、UART、配置差异与未验收边界见[ESP32 IRAM QEMU 检查点](esp32-p603-iram-capacity-qemu-checkpoint.md)。这两档均未改正式产品配置或 FRP 源仓，ESP32-C3 无同等 IRAM 区域，不能据此验收 P6-03／P7-02。

2026-09-27 Base 消费双源码签名业务包检查点：从 `esp-base@f1603f160fe5b68aec672e5bfa126244a6a7e16c` 出发，主固件及 NVS 容量探针的 Container 清单精确更新为公开 `esp-container@adef78ff28bc868e1c930b61fd232207aad31a3e`，WAMR 仍为 `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。固定 SDK 的 Component Manager 删除旧锁后分别重算 C3／ESP32 锁；除 Container 精确版本与清单摘要外，其余依赖、Container 组件内容摘要及目标均不变。主固件两份锁 SHA-256 为 `f335d8135f5993a1daeb1d4da1a91a9bbdaddc58b4cf73024a74f19ce5e16227`／`c6961c143a9f2c5a78044209c4dbe394e04c895492de295962c147a1f09b6001`；NVS 探针两份锁为 `96095165beaae57288d73b62ed8e22485913b7afd52514ebd425b5977d46e583`／`ea276091e9325b557488f019d59925093c3356985cea1345a758558a0e7c6837`。

Base 的宿主生命周期回归用锁内 Container 工具和 wasi-sdk 33 从两份无 GPIO counter 源码生成并分别签名 P1/P2。C3／ESP32 两种目标宏均在同一 boot、同一固件集合、同一 Base storage claim 中，让 P1 完成安装、确认、运行、停止及回收，事件 `{1,2,3}` 返回 3；随后 P2 经另一次五次 ECS2 提交确认，包摘要改变而固件集合不变，再次运行时相同事件返回 6。真实 WAMR、正式槽验签和 Base 产品线程均参与，Flash/NVS、调度与固件观察由宿主假件提供；同一运行还通过原有停止／卸载、100 次重装与旧 V2 收据重放及期限回归。第 10／50／100 次的 malloc 已用字节和 VM 总字节在两目标各自运行中精确相等；一次原有 macOS VM region 数断言虽多 1，但两个字节总量完全相等，现将 region 数保留在日志而以实际占用字节判断是否累积。此前测试假件仍引用已移出全局产品对象的三块工作区，本轮改由测试专有工作区承接，首次编译失败后修正并重跑通过。

固定 ESP-IDF `578cf89c`／lwIP `2758df4` 双目标普通完整构建通过：C3 未签名 `0xdefc0` B、SHA-256 `f9cdbcc33e71748d5912503002c7b501e7654f37cedefade7dba7ef0387f7187`；ESP32 显式离线探针未签名 `0xd2bb0` B、SHA-256 `a3b2236f79a26f77ed2f38d703ea3d1af59e7e55222367ef66e9c4b5af4004f7`。双目标 Base host ASan/UBSan 分别 **20/20／19/19**，NVS 容量探针双目标独立构建也通过，但没有重跑其 100 代 QEMU 写入。此轮普通构建未启用产品授权或签名，没有产生可刷写产品镜像；Base 尚无公开 `product.*` 安装入口、真实包来源、持久原 ID 结果及设备事件源，正式分区、网络并发和实体板均未验证，P6-11／P7-02 保持未验收。

2026-09-27 Base OTA authority 预检精确消费：从已提交的 MQTT 消费 `esp-base@3eae866e5d6dcfad91655587620dbdf5daebdb79` 出发，主固件唯一 `esp_ota` 清单及官方重新生成的 C3／ESP32 两份锁精确指向公开 `esp-ota@d98361f348e19e965efd7462277dde0ae13056fa`。两锁除 target 外相同，OTA component hash 为 `58dbb4b4cd22596ec9604b634b4f24f2417da77f66c57c5d0ed7ba6afce3693c`，C3／ESP32 锁 SHA-256 分别为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`／`5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`；受管 `update.c` 与上游源码 SHA-256 均为 `b3f3eec08364334135786da63a73885a32874aa1810e28909f35c2b0cc45f5dc`。上游用同一 URL authority 基本结构规则在旧 B 物理退役前拒绝空主机、空方括号主机和非法端口；它不预先证明 DNS、CA、镜像内容或网络可达。上游先用 C3／ESP32 真实更新源码及 SDK 假件的 ASan/UBSan 回归复现旧版失败，修正后固定 SDK 原生 mbedTLS HTTPS 在内的 host CTest **6/6** 通过。

本 Base 组合使用固定 ESP-IDF `578cf89c`／lwIP `2758df4`，双目标主固件从隔离 checkout 完整编译：C3 默认未签名 app `0xdefc0` 字节、SHA-256 `bd22d4615bb7a5dbbcbc9695fbb3a449a74e5c2baeef901a0ef26bb762142713`；ESP32 在独立 sdkconfig 和显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON` 下未签名 app `0xd2bc0` 字节、SHA-256 `de9a4cf385578f58e015fa961526a7db763a915d6d8a16e29b6611d81eb5b169`。官方分区尺寸门通过；Base host ASan/UBSan C3 **20/20**、ESP32 **19/19** 通过。未使用签名键、网络凭据或设备，未生成本精确组合的签名产品镜像，也未执行 HTTPS／Flash／bootloader 的实体升级、断电恢复或五能力并发；P5、P6-03、P7 设备与资源验收状态不变。

2026-09-27 Base MQTT 消息槽清零精确消费：主固件唯一 MQTT 清单和官方重新生成的 C3／ESP32 两份锁均指向公开 `esp-mqtt@c0677e5e779c3e51e814f2920420be7ec54f1d88`，受管源码逐字节核对，两目标完整离线构建与 Base host ASan/UBSan **20/20／19/19** 通过。其他组件、产品策略及正式分区未改；本轮未签名、联网或刷板。[精确输入、镜像与边界](mqtt-slot-clear-base-dependency-checkpoint.md)单独记账。旧锁下无认证探针的 ESP32 签名 guest QEMU 启动 minimum **43,636 B**，仍低于 **49,152 B** 门；本轮未重测资源，不把依赖更新计为 P6-03 通过。

2026-09-27 Base FRP `1660ac2` 精确消费：固定 SDK 重新生成 C3／ESP32 主固件锁并完成双目标普通、仓外测试签名构建，官方 app／分区签名及容量门通过，Base host ASan/UBSan **20/20／19/19**。NVS 探针没有 FRP 依赖，其锁保持原样，另在两目标完成全量构建。集成签名产品入口的纯 Wasm 期限宿主测试后，双目标 host 仍通过；签名 guest 超期阻断与清理、原有 100 次安装／卸载回归在新锁组合下通过。输入摘要、镜像、原始日志及未覆盖的 FRPS／实板边界见 [FRP 1660ac2 精确消费检查点](frp-1660-base-dependency-checkpoint.md)。

2026-09-27 Classic 入口期限精确消费：Base 主应用和 NVS 容量探针均在 `project()` 前启用 WAMR Classic 协作式墙钟检查。两目标各自的主固件与探针 `dependencies.lock` 精确锁定 Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`；主固件同时升级 OTA `f4fb0b4f3fa7b384edf540bac626314418156d22`。官方 Component Manager 重新生成四份锁；ESP32 曾在保留旧锁时只更新 manifest hash、继续选择旧提交，隔离副本删除旧生成锁后重新求解，日志确认全部新 SHA。固定 ESP-IDF `578cf89`／lwIP `2758df4` 的普通 C3 和 ESP32 主应用完整构建分别得到 `0xdefb0`／`0xd2bb0` 字节，官方分区尺寸门通过；两目标编译命令均确认 WAMR Classic 与 Container runtime 实际收到期限宏。普通构建不含产品签名策略，尺寸不能当作最终候选。

- 锁定的公开 Container/WAMR 源码与 wasi-sdk 33 经正式 Base 生命周期宿主入口完成真实 RSA 签名包安装、guest 启动、停止/卸载/同 boot `EMPTY` 的 100 次循环，ECS2 sequence **1→601**；ASan/UBSan 无报告。非 sanitizer 版本第 10／50／100 次后 malloc 均为 **356384 B**、虚拟字节均为 **500518191104 B**、VM region 均为 **68**。Base 全量 host ASan/UBSan 在 C3 **20/20**、ESP32 **19/19** 通过；固件集合、NVS/Flash 与 OTA SDK 故障仍由宿主替身注入。
- 仓外沿用此前已验的测试键、签名策略与候选布局，从新 Base 源码和精确三项新依赖分别构建完整产品 app：C3 RSA v2 **`0x111000`**／SHA-256 `b7e922df87d4bc691ba7247dc465bd936518b36c80797bbf4a556c0a152f05ce`，双 `0x120000` app 槽各余 `0xf000`；ESP32 ECDSA v1 **`0x10fff4`**／SHA-256 `03101ae499eb7f590940201293f8425c52b2d7df33ce539babab59b77d32feef`，各余 `0x1000c`。官方 app 验签、ESP32 分区表验签和容量门通过。原始 build/map/镜像位于 `mac-work-1:/private/tmp/esp-base-classic-deadline-signed-capacity-check-20260927/`。ESP32 这份候选沿用旧 `frp_scratch + 六页 base_store` 仓外 CSV。随后以**当前正式 ESP32 CSV** 的 `base_store@0x3ea000/0x16000` 在另一个隔离副本重建：仅将签名 sdkconfig 的 scratch 关闭及 NVS offset/size 共五行改齐，正式 CSV／锁逐字节不变；ECDSA v1 app 仍为 **`0x10fff4`**／SHA-256 `a6e6da4f7d13c9fcdde21e75b39f3881d4fd93a43deedda28fcbbb190655f98c`，官方 app／分区签名及容量门均通过，槽余 `0x1000c`，原始收据位于 `mac-work-1:/private/tmp/esp-base-classic-deadline-official-csv-signed-20260927/`。这只是正式 CSV 的隔离静态构建，不含设备安装或旧 AT 迁移。两板实物未刷写，纯 Wasm 期限的板上调度、网络/guest 同存、旧 NVS 迁移和掉电恢复仍未证明。
2026-09-27 Base `e536b3d` 产品卸载双目标 QEMU 检查点：在独立归档与测试签名镜像中，以锁定 Container/WAMR、真实签名 ABI 2 counter 包及合成 Flash 预置已确认绑定，C3／ESP32 的真实 FreeRTOS 测试任务持 Base storage claim 调用正式 `stop_confirmed → product_uninstall → product_boot`。两目标均得到停止成功、卸载 `COMPLETE`、同 boot `EMPTY`，同片 Flash 冷启动仍 `EMPTY`；ECS2 sequence 6 → 7，包区及 app 槽未变。原样签名 ELF 没有卸载调用入口，首次 ESP32 从 `app_main` GDB 直接调用曾触发 3584 B main 任务栈溢出，因此成功路径保留生产 main 栈并使用独立 16 KiB 测试任务。准备脚本、测试源码、镜像和包摘要、原始串口／GDB、逐区读回与限制见 [产品卸载双目标 QEMU 检查点](product-uninstall-qemu-checkpoint.md)。前置安装由仓外正式 Container API 离线 seed 完成；当前没有设备产品命令或 Base 安装 API，不能据此宣称设备重装、实板或 P6-03／P7-02 验收。

2026-09-27 Base 产品 guest 同进程 100 次回收检查点：从 `esp-base@b7a2ef4b0d4d4557a1c9340d3c7892f6ab260c22` 的独立工作树出发，精确消费 `esp-container@3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd`、WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5` 和 wasi-sdk 33。每轮保持同一个 Base storage claim，宿主经正式 Container 槽 API 将真实 RSA 签名 ABI 2 counter 包按 `reserve → write_and_prepare → begin_trial → mark_healthy → confirm` 安装到宿主注入的已确认固件集合；正式 Base `product_boot` 启动产品线程并经包验签、WAMR `init` 到 `RUNNING`。内部 `product_uninstall` 精确带入本轮 sequence、包 SHA 和唯一 operation UUID，要求 guest `stop`、runtime `close`、线程 `join` 与 native 回收证明，再提交 `econtainer_slots_uninstall` 并独立读回；包 Flash 未被卸载擦写。随后同 boot 正式 `product_boot` 返回 `EMPTY`，再进入下一轮。当前没有授权业务事件源，Base 线程没有执行 guest `on_event`；这也不是公开产品安装入口。

- ASan/UBSan 同一测试进程完成 100 轮且无报告；ECS2 sequence **1 → 601**，每轮安装 5 次、卸载 1 次提交，未靠近 `UINT32_MAX`。非 sanitizer macOS 二进制先校准 64 KiB 堆和 VM 分配可见，再分别在第 10／50／100 轮的卸载、join 和同 boot `EMPTY` 完成后采样：默认 malloc zone 已用字节 **361104／361104／361104**，`TASK_VM_INFO` 虚拟字节 **445752655872／445752655872／445752655872**，region **65／67／67**。malloc 与虚拟字节三点相等；region 从第 10 至 50 轮增加 2，第 50 至 100 轮持平。原始输出 SHA-256 `1ae2972278495892726e09bc473a20976df138de8cf627a5fa6aea40bc45f4d4`，位于 `mac-ci-1:/private/tmp/esp-base-container-100-cycle-lifecycle-20260927.log`。这是宿主分配和线程回收的三点观察，不能推断 ESP 设备堆、任务栈、Flash 耐久或实板 100 次运行。
- 同源码在 `mac-work-1` 独立副本复用精确锁定的受管组件运行全量 host ASan/UBSan：C3 **20/20**、ESP32 **19/19**，无 sanitizer 报告。原始日志分别为 `/private/tmp/esp-base-container-100-cycle-host-20260927/host-esp32c3.log`（SHA-256 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`）与 `host-esp32.log`（SHA-256 `e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`）。该全量入口不运行签名 counter 循环；循环由上条独立入口验证。Flash/NVS 和签名固件观察在循环中仍为宿主替身，没有实体设备、掉电或业务事件验收。

2026-09-27 在最新 Base `bdf1647`／Container `3b5f16f` 卸载集成上，仓外条件性 C3 几何与测试键复建的签名 app（SHA-256 `c994cfc1ae04095c72cf5f2bc65c9af1cdaba36e958976d4f0f9fdf13cdb9a3f`）及独立签名 ABI 2 包，两次 QEMU 冷启动均完成正式产品 guest `init`、`RUNNING` 与 Base `READY`，`base_store`／包区二启未重写。首次未经候选 OTA policy 两行改写的构建因正式 policy 与仓外缩小的 app 槽不匹配，在 ECS2 读取前返回 `SLOT_UNAVAILABLE`，失败收据也已保留。候选源码差异、两轮 heap 与精确 SHA 见 [C3 产品卸载版本检查点](c3-product-uninstall-branch-qemu-checkpoint.md)。这是仓外预置包的启动证据，尚未执行卸载或设备安装入口，正式 CSV／policy／实体板未改。

2026-09-27 ESP32 签名产品包仓外 QEMU 检查点：从 Base `bdf164750410f7a7f030fa3dbd188ae94c4823c3` 精确归档独立构建固定 SDK 的 ECDSA v1 app，官方验签与双 `0x120000` 槽容量门通过；仓外产品策略、候选 CSV 与真实 RSA 签名 ABI 2 counter 包的公钥、产品 ID、授权和 `0x260000` 包区几何逐项匹配。锁定 `esp-container@3b5f16f` 的正式 slots／validator 在 ASan/UBSan 下离线生成 ECS2 sequence 6 `CONFIRMED`，将**新签名 app 摘要**绑定到唯一 running 固件，再以官方 V2 NVS generator 预置六页 `base_store`。固定 Xtensa QEMU 的同片两次冷启动均经正式产品线程 `init` 到达 `RUNNING` 与 Base `READY`；首轮仅系统 NVS 101 B、otadata 12 B 变化，第二轮整片 Flash 不再变化，ECS2 和包区字节保持原样。GDB 在 READY 时读取运行态内部空闲／最大连续块 **47,548／26,624 B**。精确输入、官方验签、两轮原始日志位置、逐区读回与限制见 [ESP32 签名产品包 QEMU 检查点](esp32-signed-product-qemu-checkpoint.md)。仓外 seed 不等于公开安装；没有实体设备、业务事件、停止卸载、网络并发或掉电验收。

2026-09-27 C3 仓外合成签名包已由 Base `d3144b3` 的真实产品启动线程在 QEMU 中验签、`init` 并到达 `READY`；首次与同片二次冷启动的 ECS2、包区均未重复写入。无包与运行态堆量测、精确组件锁／输入 SHA、调试停止但未 join 的边界见 [C3 签名产品包 QEMU 检查点](c3-signed-product-qemu-checkpoint.md)。这是 `26c83d0` 卸载集成之前的签名镜像及仓外预置包，不含公开安装入口、正式停止重开或实体设备验收。

2026-09-27 Base 内部产品专属卸载切片：在 `esp-base@d3144b3a7507eaddd308777b863214fa53b11fcb` 之上，将主固件及 NVS 容量探针的两目标清单和 Component Manager 生成锁精确提升到 `esp-container@3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd`。Base 只有内部 `esp_base_container_product_uninstall`，没有 USB/MQTT/Tool `product.*` 写入口或设备端 `product.result`。调用方先持唯一 Base storage claim、预检当前签名固件集合、精确 ECS2 sequence／包 SHA／新 operation UUID，再对运行中的已确认 guest 停止、关闭并 join，或对已停止／启动失败 guest 验证 worker 已 join 和 native 资源确已回收；Container 公开 API 清除运行固件的包绑定，Base 再独立读取 ECS2 并确认回退固件绑定逐字段未变。成功后同 boot 正式 `product_boot` 返回 `EMPTY`；运行中失去 ECS2 key、停止、提交、独立读回或固件观察不确定均保持本 boot 重开关闭。当前包 Flash、回退包字节和数据未擦除，ECS2 最近无包 operation 不保留旧包 SHA/长度，未来跨 boot 请求结果关联仍需控制器持久账本。

- 固定 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`／wasi-sdk 33 的真实签名 counter 包 host 集成在 Base ASan/UBSan、`halt_on_error=1` 下通过：双固件都有签名包时只卸载当前固件、回退引用及整份 Flash 字节不变；错误 sequence／摘要在停止前拒绝；运行中 ECS2 key 丢失、Container 提交读回与 Base 独立读回故障、唯一 pthread 停止超时均不开放同 boot 重启；已 STOPPED 与损坏包启动失败但 native 确已回收的确认绑定可卸载；旧 OTA V2 `SUCCEEDED` 收据在卸载推进 ECS2 sequence 后仍按原 A/C 核对。C3／ESP32 全套 Base host ASan/UBSan 分别 20/20、19/19 通过且无 sanitizer 报告，原始日志保存在 `mac-work-1:/private/tmp/esp-base-product-uninstall-candidate-20260927/host-esp32c3.log` 与 `host-esp32.log`。
- 固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，只在仓外使用既有测试键、签名 sdkconfig、C3 候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601` 和 ESP32 候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`。C3 官方 RSA v2 验签和容量门通过，签名 app `0x111000`、SHA-256 `9ac0f39def33ad15cc304a4f0943df535dd388430a58aee8e7192a78cec8a53b`，双 `0x120000` 槽各余 `0xf000`；ESP32 官方 ECDSA v1 验签和容量门通过，签名 app `0x10fff4`、SHA-256 `df4596c5577dadcc149ebce38772fa17d3c1f2aff70e3d8da803975144eb209b`，各余 `0x1000c`。Component Manager 生成的 Container component hash 为 `46285191cc8401b7892e3c1656aaf2279e03d902563880741f5011d0687a0773`；主固件和容量探针的双目标锁分别只差 target。构建、官方验签与锁重配日志保存在同一仓外目录，未改变正式 CSV、密钥或实体设备。本轮没有实板业务包执行、掉电与网络并发验收。

2026-09-27 前一阶段软件集成 `esp-base@9967b3a2d77a4a3889df9bff67f0b7f93d01d206`：产品执行线程在真实停止、关闭、回收后允许同次启动再次接入；`EMPTY` 只有在新签名包真实写入且仍持有 Base 存储 owner 时才重新打开，失败、阻断和超时状态不得重开。无包 OTA 的 `SUCCEEDED` 收据在后续独立产品绑定提交后仍按原固件 A/C、operation 与 ECS2 序号核对，不把较新的非固件提交误判为 OTA 收据漂移。固定 WAMR／wasi-sdk 的真实签名 counter 包完成无包安装、trial、confirm、执行、停止和同次重开，以及旧成功收据与较新产品提交、损坏包和停止超时的 host 故障注入；C3／ESP32 源码编译通过。Container 新增的产品专属卸载入口仍在独立 `esp-container@3b5f16f` 分支，该阶段 Base 精确锁 `bf52b17` 尚未消费它。

- C3：在原有 WAMR／MQTT／FRP 局部 LTO 基础上，仅给 `container_binding` 与 `esp_container` 增加局部 LTO，避免当前新代码使签名应用越过容量门。使用与本提交完全相同的 CMake SHA-256 `02295ea5e6a5edfe7524dfbf73ddf3bd6ad3908d8df2f301606a213927a425b9`、固定 IDF／lwIP、仓外几何 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601` 与测试 RSA v2 键，并在隔离构建副本中将 C3 OTA policy 两宏条件性改为 `ota_1@0x140000/0x120000`（候选头文件 SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`），官方签名验签及容量门通过：签名应用 `0x111000`、SHA-256 `4ab810aeb3faf545b530a556cfed037cce74666d48fc50d0942f8381a118f0a9`，双 `0x120000` 槽各余 `0xf000`。独立合成 4 MiB Flash 的 QEMU 在唯一 ADC2 校准模拟限制下，FRP scratch 恢复返回 `EFRP_OK`／IDLE、Container 为 `EMPTY` 且线程已回收、重开门就绪；Base 到 `READY` 时存储 claim 的 owner/token 为 0，无本地失败断点。精确日志与收据见 `mac-work-1:/private/tmp/esp-base-30e608f-signed-qemu-20260927/c3-verification-summary.json`。
- ESP32：同一源码及仓外候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`，官方 ECDSA v1 app／分区验签、容量门通过；签名应用 `0x10fff4`、SHA-256 `a3b9136475f035d05dbb1ef595dc2031df242a829ab22fb18993d2bda6dc0901`，双 `0x120000` 槽各余 `0x1000c`。独立合成 Flash 的 QEMU 首次与同片二次冷启动均为 `READY container=empty`，无 BLOCKED／panic；FRP scratch、包区、旧 AT 档案和双应用槽未变，`base_store` 首启写 384 B、二启不再变化，NVS 页 CRC 通过。收据见 `mac-work-1:/private/tmp/esp-base-30e608f-signed-qemu-20260927/esp32-verify/verification-summary.json`。

以上仍是仓外条件性签名布局与合成 Flash；正式 CSV、密钥、物理设备均未改，没有运行真实业务包、FRPS／Broker／OTA 同板并发或断电恢复，不据此验收 P6-03／P7-01／P7-02。

2026-09-27 当前精确 Base 集成 `620b6b1cb320bb496e945d95b504d2a88fa647a1`：FRP 唯一锁 `98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`，Container `bf52b17`，OTA `7f316c2`，固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。合并无收据恢复修正、同镜像 OTA 首写前拒绝、A/B 与 A-only 的 ECS2 最坏回滚序号门，以及正常无包 `EMPTY` 不再误打 `BLOCKED` ERROR；产品返回合同未改。从本精确 Git 归档在 `mac-work-1` 的双目标完整 host ASan/UBSan 通过，C3 **20/20**、ESP32 **19/19**，exit 均为 0；日志位于 `/private/tmp/esp-base-620b6b1-host-regression-20260927/`。以下签名与 QEMU 均使用仓外候选布局和测试键，正式 CSV、签名键、物理设备均未改。

- C3：仓外候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`、几何一致的 sdkconfig SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`。官方 RSA v2 验签及双 `0x120000` app 容量门通过，签名 app **`0x111000`**、SHA-256 `c473aba5303c48f83a3e0f2b3a4db7775594a0c3bf1523c1b54f93379778a4f2`，每槽余 `0xf000`；非填充尾 `0x10ff1c`，当前签名台阶 PADDING 仅 **172 B**，代码新增须重测。FRP 15/15、WAMR 35/35、MQTT 5/7 对象带局部 LTO，Container 不额外启用 LTO；ELF 保留 FRP Flash provider/reader、Container 产品入口与 WAMR 调用。app／分区／otadata 字节精确写入合成 4 MiB Flash（SHA-256 `65ee6bd79b86620dc8174450e2b21dfc9ff3c57d40b543b0ba0e60c05a2181a5`），包区、scratch、NVS 初始全 `0xff`。受限 C3 QEMU 唯一跳过未模拟的 ADC2 校准后，FRP scratch recover 为 `EFRP_OK`、provider IDLE／lease 0，Container 返回 `EMPTY`，Base 到 `READY` 时 boot storage owner/token 均为 0；误导性 `open_slots` ERROR 调用点断点未命中。精确输入、官方输出和原始 GDB 日志见 `mac-work-1:/private/tmp/esp-base-frp-620-final-20260927/receipt-620b6b1.json`。QEMU 的 USB 产品日志不可直接捕获，guest 未执行；这不证明 FRPS/guest/Broker/OTA 同存、真实 Flash 时延或实板。
- ESP32：仓外候选 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`、sdkconfig SHA-256 `6ddf140b2845f4e1d4d9bc69b4aad06f446715d7b3d41bfe164b1846a3e0d456` 与签名分区表的包三槽、旧 AT 档案、scratch、六页 NVS 几何逐项一致。官方 ECDSA v1 app／分区验签及双 app 容量门通过，签名 app **`0x10fff4`**、SHA-256 `ce620e173a09592cab46055c47b6a0faface20ff81145751bb1875a668226307`，每槽余 `0x1000c`；签名分区 SHA-256 `49f7ee4e5b70b3bd12cb68e3e10a8ab6d0ac180c24cfa4951b8326c28e2fd602`。合成 4 MiB Flash（初始 SHA-256 `123841483adc5333ebec76e856657b342c70fb23b43be43b63842e11b7f01a36`）在固定 Espressif Xtensa QEMU 9.2.2 首启与同片二启均达 `READY container=empty`，无 `BLOCKED` 误报；运行时 ECDSA 校验返回 0。两次启动后包区、旧 AT 档案、scratch 与双 app 原样，`base_store` 首启变化 383 B、otadata 变化 12 B 后二启稳定；官方 NVS parser 页 0 CRC OK，其余五页 Empty。精确输入、官方验签／尺寸、roundtrip、两次 UART 与 postboot 收据见 `mac-work-1:/private/tmp/esp-base-620b6b1-esp32-scratch-qemu-20260927/verification-summary.json`。QEMU 控制流证明 scratch bind/recover 和启动 owner 释放，无真实网络、旧 AT 迁移、物理 Flash、掉电或实体板证据。

上述两目标均只证明条件性仓外布局的签名产品启动到无包状态；未完成正式分区冻结或迁移，也未运行真实业务包和五能力同存，P6-03/P7-01/P7-02 仍不验收。

2026-09-27 Base 集成 `619f4a7df28c863195815788e7ef3bfba5306347` 的 OTA 首写前门：V2 注册独立复核签名 A 后拒绝 C 与 A 同摘要，返回 `failed/ota_same_image`，无新收据、worker 或旧 B 退役；历史 FAILED 收据与按原 operation 查询保持原结果。Container 快照在 V2 落盘之前按最坏回滚路径检查 ECS2 sequence 余量，A/B 需 6 次提交，A-only 需 5 次；不足时只读拒绝，边界值在 HEALTH_VERIFIED 后 `abandon`／`drop` 可恰到 `UINT32_MAX`。mac-work-1 从本精确 Git 归档及 C3／ESP32 锁运行固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的完整 host ASan/UBSan，C3 **20/20**、ESP32 **19/19**，日志分别为 `/private/tmp/esp-base-619f4a7-host-regression-20260927/host-esp32c3.log`、`host-esp32.log`。这只证明合并源码的主机故障注入；最终双目标签名产品、真实 Flash/otadata 和实板恢复仍需独立验证。

2026-09-27 C3 scratch 签名产品候选的几何更正：集成分支 `esp-base@cadc1c78b68802994baf951c13f69d86e7e22b46` 锁定 FRP `98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`、固定 IDF `578cf89`／lwIP `2758df4`，仅在仓外使用双 `0x120000` app、三份 `0x82000` 包、`frp_scratch@0x3e6000/0x10000`、八页 `base_store@0x3f6000/0x8000` 与测试键；正式分区、签名键、设备均未改。首次归档的 `sdkconfig` SHA-256 `efdc33f26b2478572335b77a8936cc89a1038bb4d43d5f7c0cae67af909893fc` 残留旧 Container 包槽 `0x258000`／`0x2da000`／`0x35c000` 和 NVS `0x138000`，虽得到官方验签、可装槽的 app `0x111000`／SHA-256 `3e50c893eb50f067894c04669719fb60f53ce89398d4ba0d6989f277987eaa40`，受限 QEMU 的产品对账在 `ota_recovery` 阻断。这份归档只记录错误几何的静态量测，不能作为可启动证据。

仅修正仓外 `sdkconfig` 的包区、三槽、NVS 五个 offset 后，用**同一** `cadc1c7` 源码完整重建：候选 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`、官方分区 bin SHA-256 `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25`、修正 sdkconfig SHA-256 `da50b245aea3dd78cc292885d0e2d68c609602e49051ca1650c2dc93c24aefb2`、RSA v2 app SHA-256 `295b864d46398a31bc63034d3f7072bca6c1455e85886e3258aea177dfc6970c`。官方验签和双槽尺寸门通过，app 仍为 `0x111000`、每槽余 `0xf000`；非填充尾 `0x10fec0`、当前签名台阶 PADDING 仅 **264 B**。仅给 FRP 库增加局部 LTO（15/15 对象），WAMR 35/35、MQTT 5/7 保持原范围，Container／Base protocol/main 不扩展；ELF 包含 FRP provider／reader、Container 产品入口和 WAMR 调用。签名 app、bootloader、分区和 otadata 合成 4 MiB C3 QEMU Flash（SHA-256 `975f9b3f8f5dff313d06fa1cf891b8ddb56ef9a5e0637969e374d27e11f6711b`），签名 app 与分区字节精确一致，包区与 scratch 初始全 `0xff`。在唯一 QEMU ADC2 校准 GDB 跳过条件下，FRP scratch boot recover 返回 `EFRP_OK`／IDLE，Container 返回 `EMPTY`，执行到 Base `READY` 时 storage claim 已释放；guest 未执行，USB 控制台不可见。精确输入、官方输出和原始日志见 `mac-work-1:/private/tmp/esp-base-frp-cadc-proof-20260927/receipt-corrected.json`。此结果只归属 `cadc1c7` 及仓外几何，同镜像 OTA 与后续序号门合并后的签名和启动需复验；真实 Flash 时延、FRPS/guest/Broker/OTA 同存、RAM、旧 NVS 迁移及掉电均未证明。

2026-09-27 ESP32 scratch 启用的签名产品候选：从同一 `esp-base@cadc1c78b68802994baf951c13f69d86e7e22b46` 精确归档，固定 SDK／lwIP、FRP `98bab0c`，只在仓外采用双 `0x120000` app、三份 `0x82000` 包、`at_old_raw@0x3e6000/0x4000`、`frp_scratch@0x3ea000/0x10000`、`base_store@0x3fa000/0x6000`、Container 测试授权与 ECDSA v1 测试键。官方 `gen_esp32part --secure v1`、完整 `idf.py build`、app／签名分区表 `espsecure verify-signature --version 1` 和 `app_check_size` 均通过。app **`0x10fff4`**、SHA-256 `0727c3f98db014a735612aea2d6bbfc7ea47629fdd7979309113c18d322db093`，双 app 槽各余 `0x1000c`；非填充尾 `0x1020d8`、本签名台阶 PADDING `0xdea0`／56,992 B。签名分区表 SHA-256 `49f7ee4e5b70b3bd12cb68e3e10a8ab6d0ac180c24cfa4951b8326c28e2fd602`，最终 sdkconfig SHA-256 `ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`；输入及官方日志在 `mac-work-1:/private/tmp/esp-base-cadc1c7-esp32-scratch-sign-20260927/`。C3 专属 FRP LTO 不改变 ESP32 产品编译范围。该静态结果不代表旧 AT 原字节迁移、六页 NVS 掉电、双签名 bootloader 回退、实体 Flash 暂存或五能力运行峰值通过；正式 ESP32 CSV、设备均未改。

2026-09-27 Container 启动收据边界修正：原产品启动在没有 V2 收据或收据已记 `FAILED` 时，仍可能凭 ECS2 自身的 `firmware_transition` 和当前固件集合执行 `abandon`／`drop`，或在 VALID C 的 `HEALTH_VERIFIED` 窗口自行 `confirm`；即使不触发写入，普通 reconcile 也可能放行无收据的 `CONFIRMED` 固件迁移。本轮从普通产品启动删除该通用恢复；`NOT_FOUND`、`FAILED`、OTA 不可用时先在 Base 存储 claim 下只读加载真实 ECS2，残留固件迁移一律阻断，允许确实缺键的无包首装与无固件迁移的包绑定。VALID C 的 `PREPARED` V2 收据若逐项匹配原 operation ID、A/C 签名摘要、原 ECS2 sequence 和 `NO_PACKAGE`，且 ECS2 为 `HEALTH_VERIFIED`，就在收据对账中调用 Container confirm 并读回；`CONFIRMED` 同一收据幂等通过。A 仍运行的中断恢复仍只消费原收据限定的物理槽和 ECS2 状态。

- 固定 SDK `578cf89`／lwIP `2758df4` 在 `mac-work-1` 的独立副本重新解析本仓 C3 精确锁；C3 与 ESP32 的 `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 均通过。新 product fake 检验错 operation、sequence、摘要零确认写入，已确认状态不重写，确认失败阻断；启动 fake 检验缺失/失败收据与 OTA 不可用时不进入产品 boot，以及原收据选中 C 的恢复接线。依赖仍为 Container `bf52b17`、OTA `7f316c2`；没有改分区、组件锁、签名键或设备。
- 本检查点仍是 host 故障注入：Container provider 的实际 NVS 掉电行为、VALID otadata 与 ECS2 提交之间复位、签名镜像读回、包的真实启动、guest 停止以及五能力同存尚未实板验证。P6-03／P7-02 保持进行中。

2026-09-27 P6-03 ESP32 六页 `base_store@0x3fa000/0x6000` 的[独立合成容量验证](esp32-six-page-nvs-capacity.md)：固定 IDF/lwIP、公开 Container ECS2、Xtensa Espressif QEMU 在仓外 4 MiB Flash 上完成 100 代最大 Base v3 配置 CAS、当前 OTA V2 形态及 ECS2 元数据同分区写读；两次新进程重启读回最终 revision 100，官方 NVS parser 核验五页 CRC 正确、一页 Empty。测试专用 factory app 不是双槽签名产品，正式分区、旧 AT 迁移、掉电与实体板均未验证，P6-03 保持未验收。

2026-09-27 双目标签名产品局部 LTO 容量账本：在 `esp-base@8a62d27`、FRP `6609fbc`、MQTT `9d6d95e`、OTA `7f316c2`、Container `bf52b17`、WAMR `26c235e`、固定 IDF `578cf89`／lwIP `2758df4` 及同一仓外签名输入下，只给指定静态库增加 `-flto`，逐档完整重建并经官方签名验证、双 app 槽尺寸检查。没有改变组件源码、锁、正式分区或实体设备；以下均是**当前代码的仓外优化空间实验**，未来接入 FRP Flash reader、session 和 Base provider 后必须重做完整产品门。

- C3 原始产品 RSA v2 app 为 `0x111000`，PADDING／当前签名台阶可吸收的非填充增长只有 **888 B**。在原有 WAMR、MQTT 局部 LTO 上新增 FRP、Container、`device_protocol`、`container_binding`、`ota_operation` 后仍为 `0x111000`，PADDING 增至 **7,656 B**；把其余 Base 自有库与 main 也纳入后最多 **8,384 B**。每档官方 RSA 验签和两个 `0x118000` app 尺寸门通过。超过当前台阶会跳至 `0x121000` 并溢出每槽 `0x9000`，不能把 `0x7000` 槽总余量误当作任意新代码预算。
- ESP32 原始产品 ECDSA v1 app 为 `0x10fff4`。FRP、Container、全部 Base 自有库、MQTT、esp-ota、cJSON 的局部 LTO 把当前镜像降到 **`0xffff4`**，官方验签与原双 `0x120000` app 及仓外双 `0x110000` app 尺寸门均通过；但较小签名台阶的 PADDING 仅 **684 B**，新增 provider/session 很可能使镜像回跳。Xtensa 下给 WAMR 启用 LTO 的组合因 `dangerous relocation` 链接失败，不能采用。固定 SDK `gen_esp32part.py --secure v1` 拒绝 `0x118000` app 槽大小，不能用中间尺寸绕开布局约束。独立实验账本及每档输入/签名镜像/失败日志保留于 `mac-work-1:/private/tmp/esp-base-lto-headroom-20260927/`；本提交只记录可复核的代码与分区边界，不把仓外 CMake 实验变成正式编译配置。

2026-09-27 FRP Flash reader 的 Base 隔离接线候选：从 `esp-base@f2d8b3623d60ca18b332c0a5f9913d50f406634d` 出发，唯一公开 FRP 锁更新至 `esp-frp@98bab0c0fbac684a6f89772c50c8bcf37aafe4fc`，FRP IDF provider 只在源仓实现。Base 的静态 provider 以显式 label/type/subtype/offset/size 绑定，在 storage owner 初始化后、任何 OTA pending 确认前调用 boot recover；recover/begin/write/read 的每次 Flash I/O 经现有全局 owner 取得并释放短 claim。已恢复 store 经 protocol、FRP owner 传给 client，生命周期直到 client 销毁；`clear` 只撤销 RAM lease，不占 owner 或重复擦除。无 store 时 USB `config.set` 拒绝新启用 FRP，返回 `failed/frp_storage_unavailable` 且不改变旧配置/revision；MQTT 同入口继续先返回 `failed/physical_usb_required`。已存 FRP 配置仅报告 failed，不自动改 NVS。开关默认关闭，正式分区 CSV、签名策略、设备均未修改。两目标准确锁的 SHA-256 为 C3 `9749bc4ed8ed821acc290b35dfb13e6d92a5eb7c9b4af380fd06ba7db3f42eae`、ESP32 `8683f58aed4588729364117b94bc93ab99a11ec6554bba2cbe126f2c8d839691`，除 `target` 外内容一致；FRP component hash `4c176eff15bc4d4003178053ec52f5a2d9a0085e4fe85429942a3b21b83a9197`。

- `mac-work-1` 固定 ESP-IDF `578cf89`／lwIP `2758df4` 下，C3 和 ESP32 的 Base host ASan/UBSan 全套均通过；C3 还运行 `ota_startup_scratch_test`，用假 provider 验证绑定/恢复失败早于 NVS 与 OTA pending 确认，恢复回调持有 owner 且返回后释放。FRP 源仓的假 Flash/owner 测试负责真实 provider 的几何、lease、短读写与 OTA owner BUSY 路径；Base 假件不替代真实 provider 或实板。C3 默认正式分区、scratch 关闭的普通固定 SDK 构建通过，镜像 `0xdec40`、SHA-256 `461d2c7056e330db67bb7cf1d1d38a097fa7799ba66ba8585b0a191834ba7983`。host 和构建日志在 `mac-work-1:/private/tmp/esp-base-frp-scratch-provider-20260927/`。
- 本轮真实 provider 启用的 C3 完整签名产品容量探针仅在 `mac-work-1:/private/tmp/esp-base-frp-final-candidate-20260927/` 的**本次 Base 全量源码**副本运行，只有仓外分区、OTA policy 几何和签名 sdkconfig 覆盖。仓外 CSV SHA-256 `73a36f6c55ac26d904d5dc3c48eecdb1f12d10152b3e746686ab28cd237c0601`、官方分区 bin SHA-256 `8e5c4eea7d5cf692ac9f5188e778cdfe77fb3806cbf13e04f9b305cac9b9af25`：`ota_0@0x20000/0x120000`、`ota_1@0x140000/0x120000`、`product_pkgs@0x260000/0x186000`、`frp_scratch@0x3e6000/0x10000`、`base_store@0x3f6000/0x8000`。仓外 OTA policy 头 SHA-256 `1541d9bdd8eab8ad9e0322988a0f28c6a84c706934e9dcd199cdaf5af8532944`，最终 sdkconfig SHA-256 `efdc33f26b2478572335b77a8936cc89a1038bb4d43d5f7c0cae67af909893fc`。固定 SDK 完整编译和 RSA v2 签名已完成，app `0x121000`、SHA-256 `0f53ec3dc3de9addc9a17ad24f2b3d99d67bd26bfd40d2f0b81ace330f77d670`；官方 `espsecure verify-signature --version 2` 验证 block 0 有效，但官方 `app_check_size` 判双 `0x120000` 槽各超 `0x1000`，**容量门失败，镜像不可装槽**。它不是旧双 `0x118000` LTO 账本表；旧表若装同镜像将各超 `0x9000`。先保留这次失败事实，待 Container 收据边界修复合并后按最终源码实测最小 LTO，不把旧代码容量补偿预设为正式配置。
- ESP32 当前既有双 `0x120000` 产品表、scratch 关闭的签名构建通过，ECDSA v1 app `0x10fff4`、SHA-256 `50ff51b1be33f78901e80503f30d6fb9d6f7fe39df2eb5896f5522df53e29829`，官方验签与尺寸门通过，各槽余 `0x1000c`；分区 bin SHA-256 `511ecfdd8df96b3765d5844ba9e76d3025ab9008949046951f44eae771b236eb`、最终 sdkconfig SHA-256 `618051e5a2b88376d820f65ac7acd9288f88e801720c6b9aeb89baebc117caecda`。此构建不含已绑定 scratch，不能作为 ESP32 FRP 大记录可用或 P6-03 并发验收。FRP 大记录在 OTA 长持全局 owner 时可能因 BUSY 安全失败，clear 可撤销 lease 以关闭 session；小记录走 RAM 不占 Flash owner。当前仅证实 fail-closed 安全切片，没有 FRP/OTA 同时活跃、物理 Flash 掉电、真实网络、guest 或实体板验证。

2026-09-27 C3 精确签名产品镜像的 QEMU 启动诊断：本轮只复用 `esp-base@8a62d27be4ee5d6c30ed423b6237e0582ceb9e95`、FRP `6609fbc9b324c5e10615731a1a994ec0bfacd258` 的仓外构建 `mac-work-1:/private/tmp/esp-base-frp6609-product-exact-20260927/build-c3/`，其签名 sdkconfig SHA-256 为 `a5a6149f3250e4053e582e4de0b691cc54bbfdf9d2626ac05c7e5e93b83148e8`，分区二进制 SHA-256 为 `59b08d9ea5254705338811b053a49eea273eac685c30295c017db8431754492a`。原始测试键 RSA v2 app SHA-256 为 `041e0661554ee5e9d1b91eaec6e9e659f6db12803e3606330860ebe2743ee6b5`；官方 `espsecure verify-signature --version 2` 验证成功，官方 `app_check_size` 报 `0x111000`、最小 app 槽 `0x118000`、余 `0x7000`。使用该构建自身的 bootloader、分区、otadata 和 app 合成 4 MiB QEMU Flash，SHA-256 为 `880375f3cb869025915480d5fadb2846f6a9dd8c396bb06ea0899b50f565bd1f`；`0x20000` 起的 app 字节与原签名文件逐字节一致，三个 product 包槽所在的 `product_pkgs@0x258000/0x186000` 全为 `0xFF`。

- 先前 `esp-base@299851f` 镜像的 `idf.py qemu` 日志只有 ROM `entry`，原因包含两个独立的 QEMU 观察／执行限制：生产 C3 配置把控制台设为 USB Serial/JTAG，而所用 Espressif QEMU `9.2.2 esp_develop_9.2.2_20260417` 的 `-serial` 接在 UART0，USB Serial/JTAG 设备没有可指定的字符输出后端；此外固定 IDF 的 Wi-Fi 启动会引入 `adc2_cal_include()` 构造器，C3 QEMU 未模拟其 ADC2 校准完成。该旧镜像 `ad719fd0...` 在 QMP 停止后 PC `0x4203b326` 映射到 `adc_ll.h:991`，本轮精确 `8a62d27` 镜像的 GDB 也在 `adc2_init_code_calibration` 入口命中。旧 `ad719fd0...` 不是本段当前 FRP 锁的运行证据。
- 为检验当前原始签名镜像，在 `mac-work-1:/private/tmp/esp-base-c3-qemu-boot-diagnose-20260927/` 只复制合成 Flash／默认 QEMU eFuse 文件并开启 GDB；在 `adc2_init_code_calibration` 入口只将模拟 CPU 的 `pc` 设为其返回地址，跳过 QEMU 无法完成的校准，然后继续运行。没有重编、重签或修改原 app、bootloader、分区、sdkconfig、SDK、受管组件源码、实体 Flash 或 eFuse。GDB 顺序到达 `app_main`、`esp_base_container_product_boot` 和 `esp_base_main.c:346` 的 `ESP_BASE_READY` 路径；Container boot 返回 `ESP_BASE_CONTAINER_EMPTY`（`a0=2`），到 READY 时保存的返回值为 `2`，启动存储 claim 已清为 `{owner=0, token=0}`，未命中本地失败断点。精确输入与原始 GDB 命令／输出保存在该仓外目录的 `exact-8a-flash.bin`、`gdb-8a.cmd`、`gdb-8a-gdb.log` 和 `gdb-8a-qemu.log`。
- 因包槽为空，**没有 guest 加载或调用证据**；READY 是带测试签名产品构建在上述 QEMU 专用 ADC2 跳过条件下的 Base 启动路径证据，不代表生产配置原样可由 QEMU 输出日志，更不替代实板启动、USB、FRPS／Broker／HTTPS 同存、运行堆峰值、三包保留或迁移验收。仓外另试的 UART0 控制台加 ADC2 空桩变体虽完成 RSA 签名，但变体 app 为 `0x121000`，官方尺寸门报告超出两个 `0x118000` 槽各 `0x9000`；没有用该变体冒充原镜像的 READY 结果。P6-03／P7-02 仍未完成。

2026-09-27 Base 产品锁接入 FRP 握手缓冲复用版本：独立集成分支以 C3 选择性 LTO 提交 `299851f9fbc26b583e79895bfc900ff070a1e8cd` 为源，唯一 FRP 清单改为公开 `esp-frp@6609fbc9b324c5e10615731a1a994ec0bfacd258`，从空解析的 C3／ESP32 锁 SHA-256 分别为 `275ea222a0fce6ad678b31334e914447d3619c1a164acb5d53994372ca7c7945`／`54c4542b418f8e01844ec45653972acf3e506ffa13f9503f38321cec02e329df`；两锁除 `target` 外相同，FRP 组件摘要均为 `6c433b36f6e11d085454aaf4584be9b772e1e68364fc10b6a1ed40a294bc6bf7`。MQTT `9d6d95e`、OTA `7f316c2`、Container `bf52b17`、WAMR `26c235e` 及固定 IDF `578cf89`／lwIP `2758df4` 未变。以下旧提交与镜像数字保留各自时点事实，不代表当前锁。

从本分支源码精确归档，在仓外使用既有 C3 产品测试策略、双 `0x118000` app 候选表及只限此副本的 OTA policy 地址／尺寸覆盖，并使用既有 ESP32 产品测试策略与双 `0x120000` app 候选表；固定 SDK 完整构建分别完成 1126／1126 和 1150／1150。C3 测试键 RSA v2 镜像 `0x111000`、SHA-256 `041e0661554ee5e9d1b91eaec6e9e659f6db12803e3606330860ebe2743ee6b5`，官方签名验证和尺寸门通过，各 app 槽余 `0x7000`；PADDING 起点 `0x10fc50`，到当前签名台阶边界的有效内容余量约 888 B。ESP32 测试键 ECDSA v1 镜像 `0x10fff4`、SHA-256 `79173ca989ec55e77bf2bfca855f183e4e764b3e4f16e8d060cd92e93bff9832`，官方签名验证和尺寸门通过，各 app 槽余 `0x1000c`；PADDING 起点 `0x101514`。同一仓外副本的 C3／ESP32 host ASan／UBSan 全套均通过。原始构建、验证与 host 日志在 `mac-work-1:/private/tmp/esp-base-frp6609-product-exact-20260927/`；C3／ESP32 仓外 sdkconfig SHA-256 分别为 `a5a6149f3250e4053e582e4de0b691cc54bbfdf9d2626ac05c7e5e93b83148e8`／`863fecff0ae8d682a33ef3eeac96db354973e0c5a097a28f44ac4c4b7c8ef41c`。这些测试输入和签名键不入库，正式 C3 分区、产品授权与实体设备未修改；签名静态装槽不能替代 QEMU/实板的 guest、FRPS、Broker、HTTPS 同存峰值及迁移验证，P6-03／P7-02 保持进行中。

2026-09-27 C3 完整产品签名镜像的选择性链接优化：以 `esp-base@299851f9fbc26b583e79895bfc900ff070a1e8cd` 的精确归档、固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、C3 锁 SHA-256 `ec4013e8f1aa081e56f93a146c2fe7e2b606f2176893bff3a571cd06007db969` 和原仓外产品测试策略构建。Base CMake SHA-256 为 `d19f867ece5999f3fd1dd8eaac231b050a1c89732768749a1a24046014d88e00`，仅在 `esp_base`、`esp32c3`、签名且产品 ID 非空时给 WAMR、MQTT 两个静态库及应用链接加 `-flto`；二者各自单独启用都不能跨签名台阶，两库同时启用才跨过本次签名尺寸台阶。没有修改 SDK、受管依赖源码、生成锁、RSA 验签、WAMR normal loader／指令计量、网络功能或正式分区。仓外候选分区表 SHA-256 `60a9d9fea67bf5ebc4afcf811ddb7216c621ccd4dd35ffa0c781930179e84404`、C3 OTA policy 覆盖 SHA-256 `7e80ce8d0a5890176a06d867325686a9b8cc54ffa0660e20d9889be915675543`、签名产品 sdkconfig SHA-256 `a5a6149f3250e4053e582e4de0b691cc54bbfdf9d2626ac05c7e5e93b83148e8`；这些测试输入与测试键仍只在 `mac-work-1:/private/tmp/esp-base-c3-lto-size-20260927/`，没有进入仓库或实体设备。

- 固定 SDK 的完整 C3 构建、官方 `app_check_size` 和 RSA v2 `espsecure verify-signature` 通过。测试键镜像为 `0x111000`／1,118,208 B，SHA-256 `ad719fd0bca84d680cb416dff307cf6969c91c1d01dd47a483ddddbca8df7e11`；候选双 `0x118000` app 槽各余 `0x7000`／28,672 B。相同源码、锁和策略下原镜像 PADDING 起点 `0x111730`，新起点 `0x10fca4`，有效内容减少 `0x1a8c`／6,796 B，其中 `.flash.text` 减少 6,036 B、`.flash.rodata` 减少 760 B；跨回较小 RSA 签名台阶后，有效内容到台阶仅余 804 B。`.iram0.text`、`.dram0.data`、`.flash.appdesc` 尺寸不变，链接脚本的 orphan 拒绝与断言通过；`econtainer_product_open`、WAMR load／instantiate／call、`emqtt_start`、FRP、OTA 及证书包符号均仍在 ELF。
- 仓外备选试验将同一局部 LTO 范围增加 `esp-frp` 库后，签名尺寸仍为 `0x111000`，PADDING 起点提前到 `0x10f54c`，到下一签名台阶的有效内容余量增至 2,684 B；该试验没有写入 Base CMake。后续 FRP 或 record reader 源码变动可能使当前仅 804 B 的余量耗尽，必须在更新精确锁后重做完整签名容量门。
- 同一源码归档的默认无包签名 C3 和 ESP32 分别完整构建并由官方尺寸门与 RSA v2／ECDSA v1 验签：C3 为 `0x101000`，ESP32 为 `0xefff4`；各自 sdkconfig SHA-256 为 `92f64c3c1d2507b94d6a26643f62e5454a415d10f2d279c86702b556e5dced15` 和 `9069e26b4cdb757f41f3593f9eb6203d94d7683e640de9ffd65c15e10e86ebfe`。两个无产品 ID 构建的生成配置均为 `CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID=""`，编译命令没有 `-flto`；带包 C3 配置为 `"esp-base-capacity-test"`，只有 WAMR、MQTT 两库的 40 个对象带 `-flto`。当前 C3 host ASan／UBSan 全套通过。
- 同一签名产品镜像的 QEMU 在 28 秒观察内只输出 ROM 加载与 bootloader 入口，没有 bootloader 日志或 Base READY；同 SHA 源码的默认无包签名 C3 控制镜像也停在相同入口。两者 bootloader 均配置 INFO 日志；产品镜像的 RSA 签名、分区二进制与合并 Flash 输入已由官方工具核对，但 QEMU／bootloader 启动条件仍未查明。因此本轮没有同镜像的 guest 调用、运行堆或联网证据，不能把静态尺寸合格解释成产品运行或 P6-03／P7-02 验收。原始日志 `build-exact-product.log`、`host-exact.log`、`qemu-exact-product.log`、`qemu-c3-noproduct.log` 与构建制品均保存在上述仓外目录；FRP 后续若改动精确依赖，必须重新生成锁、签名并检查签名台阶。

2026-09-27 当前 Base 产品入口的仓外测试授权静态深链接：以 `esp-base@3df1c33ced84a5e6778776e9888103f81d86d5ca`、本节 C3/ESP32 精确依赖锁、固定 SDK 和既有仓外测试键／产品公钥，在独立副本启用 Container 完整产品策略；两个 ELF 均实际包含 `econtainer_product_open` 与 WAMR 的 load、instantiate、call 入口。ESP32 使用当前双 `0x120000` app、三 `0x82000` 包槽的候选表，测试键 ECDSA v1 签名镜像 **1,114,100 B／`0x10fff4`**、SHA-256 `7b916acd7ef1367e6f340d72310bbb5e0556f2d6c6af7fe0a52c5ac37784c967`，官方验签通过，各 app 槽余 **`0x1000c`／65,548 B**。C3 仅在仓外副本将 OTA policy 与已存在的候选分区表对齐到双 `0x118000` app、三 `0x82000` 包槽；测试键 RSA v2 镜像 **1,183,744 B／`0x121000`**、SHA-256 `a23230b528a35040bd83903b67bca3dfe136f2afff5a64a96f0d52eeec510c68`，第 0 签名块有效，但官方 `app_check_size` 判定每槽超出 **`0x9000`／36,864 B**，所以该 C3 候选不是可用构建。仓外输入、构建和验签证据保存在 `mac-work-1:/private/tmp/esp-base-product-link-recovery-20260927/`；本仓产品源码、正式分区表和设备均未因此修改。此结果只关闭默认配置未深链接的静态证据缺口；未运行真实授权包、guest、FRP/MQTT/OTA 并发或实体板，P6-03/P7-02 不验收。

2026-09-27 C3 深链接尺寸归因补充：固定 SDK 的 `esp_idf_size --archives --diff` 用同一 Base 源码的默认 C3 签名 map 与上述完整产品 map 比较，新增主体为 WAMR **56,311 B**、`esp-container` **18,451 B** 与 Base Container binding **5,646 B**；`esp_stdio` 表观增加约 10 KiB 实为 DROM 对齐洞，其对象实际仅 `0x51` B，不能当作可删除模块。官方 `esptool image-info` 显示产品镜像有效内容后 PADDING 段始于 `0x111730`，补齐到 `0x120000` 再加 RSA v2 签名扇区才形成 `0x121000`。要回到可装入 `0x118000` app 的上一签名台阶，PADDING 起点至少需降到约 `0x10ffc8`，即减少 **至少 5,992 B 并留裕量**；预计签名镜像为 `0x111000`，但这只是门槛推算，不是成功构建。当前已启用 size 优化与 WAMR release/classic/normal，尚未证实有保持解释器、loader、诊断和业务合同的 6 KiB 删除项。C3 的运行堆缺口另行存在，不会因静态镜像跨台阶自动消失。

2026-09-27 Base 无包固件 OTA 中断恢复软件候选：当前集成源码将 `esp-ota@7f316c2a3a71dcae234b046905aee60696a357d3`、`esp-container@bf52b17a26e51d35a261bf852ac0c9cde76adefc` 和 `esp-frp@e5a6b0b5a8f6c908cddd948a3e652045022f5fb6` 写入 C3/ESP32 独立组件锁；MQTT 仍为 `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`，固定 IDF/lwIP 不变。两锁除 `target` 字段外完全一致：C3 锁 SHA-256 `ec4013e8f1aa081e56f93a146c2fe7e2b606f2176893bff3a571cd06007db969`，ESP32 锁 SHA-256 `b64c284ec81c189b27ba8d8ab63273adf483f7e20e62d5e269c991c55b00ef0d`。`base_store/base_ota/operation` V2 收据在 OTA 库同源 HTTPS URL／最小镜像头长度校验后、任何 inactive app 擦写前保存并回读签名 A、原独立 B、目标 C 的摘要、物理槽、C 长度和已对账 ECS2 sequence。worker 以原收据限定确切目标，先物理擦除旧 B 首扇区、回读镜像首字节 `0xff` 并使 B 的 otadata 失效，随后把 Container A/B 持久绑定退役成 A-only；只有这些步骤成功才下载 C、stage `NO_PACKAGE` 并选槽。若签名 A 仍运行，重启启动 claim 在产品装载前凭原收据清理部分 C、对账 ECS2 并持久记失败；若目标 C 已被选中并运行，复核 pending/VALID、C 与旧 A 的完整签名、IDF 回退资格；配置 Container 时还核对原 operation、A/C 身份与 ECS2 sequence，不清除运行镜像。产品确认后才持久写入读回 V2 `SUCCEEDED`，之前 `ota.result` 保持 unknown。启动恢复前配置写入与 MQTT/FRP owner 保持关闭；旧 V1、损坏收据和不一致状态失败保守阻断。

- C3 与 ESP32 的 Base 完整 host ASan/UBSan 套件在当前集成精确锁下通过，包含 V2 收据、物理 A-only 观察、Container 无包退役/恢复以及启动和 worker 故障注入。固定 SDK 的普通 C3 构建为 `0xde630` 字节、SHA-256 `61e2d55e2d5d9278c006ef8f9f629f802d3200c4b751ff575624903de0bfe223`，`0x1e0000` 槽余 `0x1019d0`；显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON` 的未签名 ESP32 离线构建为 `0xd2280` 字节、SHA-256 `6530735d62254ab93466eb557e787f8eb2670aa9cb8c445434013390da5dd112`，`0x120000` 槽余 `0x4dd80`，该离线镜像不得刷写。
- 仓外测试键签名 C3 镜像为 1,052,672 B（`0x101000`）、SHA-256 `3d1004be60e8f156f7f8ca1b9139a5a761fa98832762919871e2afc0a92f99a0`，`0x1e0000` 槽余 `0xdf000`，固定 SDK 的 `espsecure verify-signature --version 2` RSA 验签通过。仓外测试键签名 ESP32 镜像为 983,028 B（`0xefff4`）、SHA-256 `544b315bb8aea6ca620f3f517c24923580f46673b9083898e6fa0d15b3d0c566`，`0x120000` 槽余 `0x3000c`，固定 SDK 的 `espsecure verify-signature --version 1` ECDSA 验签通过。两个签名 ELF 经交叉 `nm` 确认实际链接 `eota_validate_image_request`、`eota_prepare`、`eota_retire_inactive`、`eota_select`、收据成功提交与 Container selected 复核符号；测试键制品不是生产签名或刷板候选。完整 host 与四项离线构建日志位于 `mac-work-1:/private/tmp/esp-base-recovery-*-final-20260927.log` 及 `esp-base-recovery-host-*-final-20260927.log`。
- 当前恢复是软件/假件与离线编译证据，没有向真实板卡写入本轮固件、执行物理电源中断或验证 Flash/NVS 提交中间态与 bootloader 后备扫描。默认 C3 无包分区及产品授权，不运行 guest；带包联合 OTA 因 Base 尚无真实业务事件来源，在写 inactive app 前拒绝。C3 的异常 `base_store` 页与 ESP32 旧 AT 迁移仍阻断设备部署；FRP 64 KiB 完整记录和五能力同板并发资源也未形成验收，P6-03/P7-02 继续未完成。

2026-09-27 pending 固件集合只读观察前置：`esp-base@16dc79ab28aeb4c30351e77fe815c9bc73401925` 把观察入口硬切为显式 `CONFIRMED` 与 `PENDING_TRIAL`。后一模式只接受运行槽 `PENDING_VERIFY`、boot selector 仍指向该槽、另一槽 `VALID` 且 IDF 可回滚；两镜像均需通过完整 signed bin 验证及 Base 项目名、芯片与分区核对，观察前后槽事实必须相同。现有 Container 适配明确仍使用 `CONFIRMED`，pending 观察只提供身份事实，不取得启动 owner、不检查包记录或授权 guest。双目标定向 host ASan/UBSan 与 Container 适配互斥测试通过；固定 SDK 使用同一 Base 提交及 FRP `1f0c8f3`、MQTT `9d6d95e`、OTA `2072731`、IDF `578cf89`／lwIP `2758df4` 的锁，从仓外空构建目录完成 C3 1072 步及 ESP32 1093 步全量编译；各自完整 Base host ASan/UBSan 套件也通过。

- C3 普通未签名 `esp_base.bin` **898,832 B**（`0xdb710`）、SHA-256 `4e22382bc876c5016d5fec795d8a0010d6c85e4800403a0f77366b16c7a0d02a`，现行 `0x1e0000` app 槽余 `0x1048f0`；生成依赖锁 SHA-256 `20ca84c413c1969b003282d2e1cc465c5ab37f6780969953f6eb83f2d7eaa6df`。ESP32 显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON` 的未签名离线镜像 **850,416 B**（`0xcf9f0`）、SHA-256 `b6746d94b97af2b02c1022543d8abf2018aee951c01bf8d16c7b0c8a58870467`，`0x120000` app 槽余 `0x50610`；生成依赖锁 SHA-256 `0884e921e13ae54164543e47c3672d1ba9d0e9fedcd2d389eaaf5c83d91a9c29`。构建和测试日志仅在 `mac-work-1:/private/tmp/esp-base-pending-firmware-observation-20260927/`。本轮没有签名新镜像、写设备或使用生产凭据；假件与编译不能替代真实 pending boot、Container 对账、包试运行或双板恢复验收。

2026-09-27 C3 新 FRP 精确锁的测试键签名离线续验：从 `esp-base@b29ef074e93ae004fb580171c33d303112b3f9c2` 建立独立源码快照，在 `mac-work-1` 的仓外临时目录使用固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、全新构建目录及临时 RSA-3072 测试键。构建后 `firmware/dependencies.lock` SHA-256 仍为 `20ca84c413c1969b003282d2e1cc465c5ab37f6780969953f6eb83f2d7eaa6df`，其中 FRP `1f0c8f37db3765a74b3b95871bb266d0c73d1248`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `207273188b984161362824c3344614e812016836` 均未改。生成的 `sdkconfig` 回读为 `esp32c3`、4 MiB 自有分区、USB Serial/JTAG、纯 STA、TLS client-only、证书包、rollback、RSA 签名应用及无硬件 Secure Boot 的升级验签；测试键路径在源码仓外。

- 签名 `esp_base.bin` 为 **1,052,672 B**（`0x101000`），SHA-256 `3aecff80a38032073299d68c8a619563354443bfd2db5bc63bce0329da6d1167`。固定 SDK 环境的 `espsecure verify-signature --version 2 --keyfile` 验证第 0 个 RSA 签名块有效。官方构建尺寸检查报告最小 app 槽 `0x1e0000`，余 `0xdf000`（913,408 B，约 46%）。官方 `gen_esp32part.py --flash-size 4MB` 对生成的二进制表验证通过：`ota_0@0x20000`、`ota_1@0x200000` 各 `0x1e0000`，`base_store@0x3e0000/0x20000`。构建、验签及 SDK 核对日志在 `mac-work-1:/private/tmp/esp-base-c3-signed-material-20260927/`；本次不接设备，不使用生产密钥，不写 Flash/eFuse，不制作正式发布制品。该证据仅证明当前精确锁的离线签名、验签与 C3 槽容量，不能证明实板旧槽迁移、OTA/回滚、FRP/MQTT 网络或五能力同板并发。

2026-09-27 ESP32 新 FRP 精确锁的测试键签名离线续验：从已推送 `esp-base@1f43b6ff867dfcc262fc6a348b6d50285395c6e0` 建立独立工作树，使用固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，保持 `firmware/dependencies.lock.esp32` SHA-256 `0884e921e13ae54164543e47c3672d1ba9d0e9fedcd2d389eaaf5c83d91a9c29` 不变；其 FRP、MQTT、OTA 精确提交分别为 `1f0c8f37db3765a74b3b95871bb266d0c73d1248`、`9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、`207273188b984161362824c3344614e812016836`。仅在 `mac-work-1` 的仓外隔离源码副本追加签名 defaults，临时生成 P-256 测试键，并从空构建目录完成 `IDF_TARGET=esp32` 签名全量构建。生成配置读回 ECDSA v1、无硬件 Secure Boot 的 boot/update 验签、签名输出、rollback、4 MiB 自有分区、纯 STA、TLS client-only；测试键未进入源码仓或提交。

- 签名 `esp_base.bin` 为 **983,028 B**（`0xefff4`），SHA-256 `1bd24b8978fe968f9392118c90684eb8834696f13a7aa082c2f2c4ff458d1390`；固定 SDK 环境的 `espsecure verify-signature --version 1 --keyfile` 报告 `Verifying 982960 bytes of data... Signature is valid.`。官方构建尺寸检查报告最小 app 槽 `0x120000`，余 `0x3000c`（196,620 B，约 17%）；bootloader `0x5b10`，距 `0x8000` 分区表余 `0x14f0`。
- 官方 `gen_esp32part.py --flash-size 4MB --secure v1` 对**生成的二进制分区表**解析与验证通过：`ota_0@0x20000`、`ota_1@0x140000`，各 1152 KiB；`product_pkgs@0x260000/1560K`、只读 `at_old_raw@0x3e6000/16K`、`base_store@0x3ea000/88K`。两槽均容纳本次签名镜像且各余 `0x3000c`。构建和验签日志保存在 `mac-work-1:/private/tmp/esp-base-esp32-signed-frp-20260927/` 的 `build-signed.log`、`verify-signature.log`；未接设备，未使用生产密钥，未写 Flash/eFuse，也未制作正式发布制品。该结果仅证明离线签名、镜像校验和容量；旧 AT 到新启动链、双有效签名槽、真实 OTA/回滚及五能力同板运行仍待受控实板验收。

2026-09-27 FRP 按密文到达分块分配的 Base 双目标锁续更：从 `esp-base@6bd532f07fc0837875aa36008c46e9c56c94b656` 的独立工作树，把普通基座唯一 FRP 清单升级到公开 `esp-frp@1f0c8f37db3765a74b3b95871bb266d0c73d1248`；MQTT `9d6d95e`、OTA `2072731` 及固定 ESP-IDF `578cf89`／lwIP `2758df4` 不变。两个目标各自从空生成锁和构建目录重新解析，FRP 组件摘要同为 `e442d31eb153efa9390ca0ff71413b9eb10ac53ddf5d9d1b7ce3cf41784754d3`；C3 `dependencies.lock` SHA-256 `20ca84c413c1969b003282d2e1cc465c5ab37f6780969953f6eb83f2d7eaa6df`，ESP32 `dependencies.lock.esp32` SHA-256 `0884e921e13ae54164543e47c3672d1ba9d0e9fedcd2d389eaaf5c83d91a9c29`，两锁除 target 外组件和版本一致。FRP 对 Base 消费的 `esp_frp.h` 接口未变；新实现仅在有效长度后的密文实际到达时逐块申请 AEAD 缓冲，未由本轮测量设备运行堆峰值。

固定 SDK 的 C3 普通构建完成 1072 步，`esp_base.bin` **898,832 B**（`0xdb710`），SHA-256 `747b3b485d25c82a02d0e2ac083944318c713cfbdd6b73ab19c37c80170e6f65`，`0x1e0000` 槽余 `0x1048f0`；ESP32 显式 `ESP_BASE_ESP32_OFFLINE_PROBE=ON` 的未签名离线构建完成 1093 步，`esp_base.bin` **850,416 B**（`0xcf9f0`），SHA-256 `dcdd7ff67aaf026a8a89b8199fa41b0564f15fcbcc3bf80a790624b7ba5ba6e0`，`0x120000` 槽余 `0x50610`。两目标 `ESP_BASE_TEST_TARGET` 的 Base host ASan/UBSan 全套均通过。此轮不生成正式签名制品，不访问串口、设备 Flash/eFuse 或生产密钥；ESP32 离线镜像不得刷写，C3/ESP32 真实 FRP 64 KiB 记录与五能力并发资源仍待设备或联合运行证据。远端构建日志在 `mac-work-1:/private/tmp/esp-base-frp-lazy-aead-{c3,esp32}-20260927/` 各自的 `build-*.log` 与 C3 副本的 `host-*.log`。

2026-09-26 ESP32 五仓可选编译续验：以 `esp-base@5fe278fef7e0b265ba83b1ace42ff181029c4863` 的独立副本，叠加公开 Container ESP32 runtime defaults，在固定 IDF/lwIP 下显式启用 `IDF_TARGET=esp32`、`ESP_BASE_CONTAINER_BINDING_PROBE=ON` 和未签名离线探针，从空构建目录完成 1141 步全量编译。隔离生成的 `dependencies.lock.esp32` SHA-256 为 `d21237f39ec05bc3bc309e7587f8915107c88fcf967ad56676c2de685478767c`，target 为 ESP32，精确锁定 FRP `36e1506`、MQTT `9d6d95e`、OTA `2072731`、Container `8eb805f` 和 WAMR `26c235e`；源码仓的常规 ESP32 锁未改。`container_binding` 静态库已产出，生成配置读回 ESP32、自有新分区表、WAMR Classic/Normal、TLS client-only，SoftAP 与 shared memory 未启用。未签名 `esp_base.bin` 为 **850,432 B**（`0xcfa00`），SHA-256 `13a12f29fc344df843c59e3b14cef77078c15e32d4906d2a2acdf41b98c9e23f`，官方尺寸检查显示 `0x120000` app 槽余 `0x50600`。主应用未链接或调用 Container 入口；此结果只证明双目标中 ESP32 的可选组件解析/编译，不是五能力运行、资源峰值、分区实写或可刷固件。

2026-09-26 P2-08/P6-03 ESP32 离线产品构建候选：从公开 `esp-base@9a837e5` 建立独立工作树，以固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`／lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 编译。C3 继续用 `dependencies.lock`（SHA-256 `229fa5d184a2e7854f7358bd04687022431ebc2caedf84867a9e3ae3b31b2a45`）；ESP32 新锁 `dependencies.lock.esp32`（SHA-256 `5686744d202dd9871f4c5aadf7d1e78ca4af02da101a23eae0e5d883d946ccc7`）仅 target 字段不同，FRP/MQTT/OTA 精确提交分别为 `36e1506a2145321fc292294de59c0aa4532f73a7`、`9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、`207273188b984161362824c3344614e812016836`。官方分区生成器以 `--flash-size 4MB --secure v1` 接受新 ESP32 表：双 `0x120000` app、`product_pkgs@0x260000/0x186000`、`at_old_raw@0x3e6000/0x4000` readonly 和 `base_store@0x3ea000/0x16000`。新增回归逐项解析真实二进制表，确认 `product_pkgs` 为 Container `econtainer_slots_idf_bind` 所查找的 `data/undefined` subtype `0x06`，且三包槽各 `0x82000` 精确排布；此检查不表示 Base 主应用已调用 provider。OTA 请求 target/scheme 为 `esp32/esp_base`/ECDSA v1 P-256，槽 `ota_0@0x20000`、`ota_1@0x140000`；C3 数值不变。

- 固定 SDK C3 普通整机 **898,816 B**、SHA-256 `33a1b1113d19d2e1b2abcf06f1cdf9f324f03570ddaf38a26f008ab280602e04`；ESP32 显式 `-DESP_BASE_ESP32_OFFLINE_PROBE=ON` 未签名离线整机 **850,416 B**、SHA-256 `13551d10220f6608c8085698e2522c7c96c3239f4ef825bd43c29fc5b682b376`。未声明离线探针的 ESP32 未签名 reconfigure 被 CMake 明确阻断。仓外临时 P-256 测试键的 ESP32 签名整机 **983,028 B**（`0xefff4`）、SHA-256 `8b0e7e40f0d11e891e9b1ed7254835b5fbccd6f33ba1afad14095d09a3d86b96`，单 app 槽剩 `0x3000c`；`espsecure verify-signature --version 1` 通过。签名构建关闭 boot 验签的负例也被 CMake 阻断，恢复配置后再次官方验签通过。生成配置回读 ECDSA、boot/update 验签、rollback、4 MiB 表、SoftAP 关闭与 TLS client-only；bootloader `0x5b10` B，位于 `0x8000` 表前。C3/ESP32 各自 host ASan/UBSan 全套通过；C3 固定 SDK 的 NVS 离线预检假件 12/12、官方 ESP32 表解析回归 1/1、旧 AT 归档合成正反例 11/11 通过。
- mac-pro-1 上两份 ESP32 旧 AT 全片备份的原件权限不满足工具 0600 输入约束，故只读复制成两份仓外 0600 独立文件再运行工具；原件未改。工具核对 4 MiB 全片字节与 SHA-256、旧分区表、`nvs@0x12000/0xe000` 和 `at_customize@0x20000/0xe0000` 前两页以外全 `0xff`，将 16 KiB 两区原始页放入仓外 0700 目录下的 0600 新文件，再逐字节及 SHA-256 核对两个**完整旧分区**重建。此归档不含旧启动镜像、otadata 或其它 Flash；两份完整恢复备份仍须保留。
- 本轮没有碰真实串口、Flash、eFuse 或生产签名键。ESP32 普通镜像只是离线探针，测试键签名镜像也不是可刷的首次迁移组合。旧 AT bootloader/otadata 到新签名 Base 的首次切换、两个有效 signed app 槽、身份/配置新 NVS、三包槽实际读写和恢复均未在板上验证；没有 ESP32 并发网络/Container 堆峰值或两板 P2-08/P6-03/P7-01 验收。`esp32/esp_base` 与 `esp_secure_boot_v1_ecdsa_p256` 是 Base 新 OTA wire 合同；只读检索当前 ESP Tool Bridge 的命令枚举只有 status/restart/config.set，HTTP 桥接不提供 ota.start/ota.result，工作区没有已实现的 OTA 投递服务端消费者。P8 仍须在真实投递端按目标构造精确签名 manifest/请求、回执及跨 boot unknown 裁决，并用两目标负例与真实 HTTPS/回滚验证，不能由 Base 离线编译计为交付。

2026-09-26 当前 FRP/OTA 锁的可选 Container 装配复验：以 `esp-base@6b18cade64984eb8c7dee0fa11ae2da2895e9cf6` 的独立副本，在固定 SDK/lwIP 下从空构建目录启用 `ESP_BASE_CONTAINER_BINDING_PROBE=ON`，叠加 C3 Container defaults，全量编译通过。隔离生成锁 SHA-256 为 `88be17761bcda9782df8077de1283a1b7e4d8974fb3ed2de0f3761482200490d`，精确包含 FRP `36e1506a2145321fc292294de59c0aa4532f73a7`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `207273188b984161362824c3344614e812016836`、Container `8eb805f3f12cb3cd836e9833acb4aca878ae80e7` 与 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`，target 为 `esp32c3`；原常规锁未改。生成配置读回 C3、rollback、SoftAP 关闭和 TLS client-only；`container_binding` 静态库已编译，普通应用镜像为 **898,816 字节**，SHA-256 `64e62ca703242847add5182d17c9e23bc6529add076d4974e99b3cc238b137cd`。主应用仍无包操作调用方；本项只证明最新五仓精确源码的可选编译接线，不证明同板并发资源、包分区、签名运行或设备闭环。

2026-09-26 FRP/OTA 精确依赖续更：Base 的唯一常规 Component Manager 清单与全新生成的 C3 `firmware/dependencies.lock` 分别固定 FRP `36e1506a2145321fc292294de59c0aa4532f73a7`、OTA `207273188b984161362824c3344614e812016836`，MQTT 保持 `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`；锁 SHA-256 为 `229fa5d184a2e7854f7358bd04687022431ebc2caedf84867a9e3ae3b31b2a45`，target 为 `esp32c3`。FRP 新提交按芯片报告登录 `arch`，OTA 新提交使准备早退清除旧 prepared 收据；本次 Base C3 固定 SDK 普通镜像 **898,816 字节**、SHA-256 `921fe16c3a469e288284494a091486036ed64c4b61490000e73eb117c74c2ff3`，生成配置仍为纯 STA、TLS client-only 与 rollback。Base host ASan/UBSan 全套通过。OTA 新提交的独立远端全套 CTest 初轮仅 4/6，两个传输时序断言另行复核；本轮没有重新构建 Base 签名镜像、ESP32 产品镜像或写板，真实签名/网络/分区迁移仍须独立验收。

2026-09-26 C3 纯 STA/TLS 客户端容量收敛：只在 `firmware/sdkconfig.defaults.esp32c3` 固定 `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n` 和 `CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y`。Base 的 Wi-Fi 装配仅使用 `WIFI_IF_STA`/`WIFI_MODE_STA`；FRP status listener 是本机 TCP/HTTP，FRP 与 OTA 的 Mbed TLS 装配显式选 `MBEDTLS_SSL_IS_CLIENT`，MQTT 走客户端传输，Container 组件没有 Wi-Fi/TLS 服务端入口。固定 SDK 的 Wi-Fi Kconfig 明示关闭 SoftAP 可节省代码，Mbed TLS Kconfig 的客户端选项只选择客户端角色；普通、签名、可选 Container 三份全新 `sdkconfig` 均读回 SoftAP `not set`、TLS client-only `y`，且目标仍为 ESP32-C3。ESP32 defaults、SDK/lwIP 与四仓精确依赖均未改，常规锁 SHA-256 仍为 `b297af1b71c58295041cbeb5e32b951cc9c34e7136c70503268e050d5089c551`，可选七依赖锁仍为 `080e996b2715c477b3c3a4fd77930e452e751dfac6eef5ed23162518db9c8cc2`。

- 普通 C3 全量构建的 `esp_base.bin` 为 898800 字节、SHA-256 `bf63fceb6d6d6f8693fc7c5dec1d0ad5a42756ef9854dcab07535f5a7255fc78`；仓外临时 RSA-3072 测试键签名 C3 为 1052672 字节（`0x101000`）、SHA-256 `6576c22a57202675341f44b2e06ae1f4937a08131986919cd695b516170781af`，`espsecure verify-signature --version 2 --keyfile` 验证第 0 个 RSA 签名块有效。可选 Container C3 独立副本为 898784 字节、SHA-256 `4c4704af191dec3473147443c5d27159be79317eedee0f78883f62853244ff7c`，其固定 WAMR profile 保持指令计量开启、bulk/shared/shrunk 关闭。三者均通过原 `0x1e0000` app 槽尺寸检查；可选构建仍仅编译 Container 组件，主应用没有包操作入口。
- Base host ASan/UBSan 全套、固定 SDK NVS 离线预检假件 12/12、串口伪终端 5/5 均通过；独立 `IDF_TARGET=esp32` 配置仍在既有产品守卫处退出。上述构建和配置读回不证明板上 Wi-Fi/TLS、五能力并行峰值、QEMU 联合运行、真实 NVS 迁移或 P7-02 验收；本轮没有刷板。

2026-09-26 五仓 C3 源码组合检查点：Base 唯一 Component Manager 清单与全新解析的常规 `firmware/dependencies.lock` 精确消费公开 FRP `533e29467b24d01157ff3b5229e62c93d101be61`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `5da4a0dfbbe97723286e1a9b050e7029cff6e718`，常规锁 SHA-256 为 `b297af1b71c58295041cbeb5e32b951cc9c34e7136c70503268e050d5089c551`。可选 Container 清单固定到 `8eb805f3f12cb3cd836e9833acb4aca878ae80e7`；该提交和后续 `46953d1a3c71f5e84ed58bef6b8837da4f8b1e8f` 的 `components/esp_container` 目录无差异。隔离可选构建全新解析的七依赖锁 SHA-256 为 `080e996b2715c477b3c3a4fd77930e452e751dfac6eef5ed23162518db9c8cc2`，其中 Container 自身精确固定 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`；常规 Base 锁不包含 Container/WAMR。SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 未改。

- 固定 SDK 的常规 ESP32-C3 全量构建通过，未签名 `esp_base.bin` 为 959712 字节、SHA-256 `79152195313db6bbe63b41abfe5c11a799f7cd7a3ff5bab80a4a5dcda449d325`。仓外临时 RSA-3072 测试键的签名 C3 全量构建为 1118208 字节、SHA-256 `5ce3ed705f1d8dec11ace789b3fbe1a51a09bfc83f121e40d7476db17b6e1a1d`；生成配置含 C3 RSA scheme、签名 app/update、证书包，`espsecure verify-signature --version 2 --keyfile` 对第 0 个 RSA 签名块验证成功。两镜像均在原 `0x1e0000` 应用槽内；测试键只留在仓外临时构建目录。
- Base `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套、固定 SDK 环境的离线预检假件 12/12、通用串口伪终端 5/5 均通过。可选 C3 探针启用 `ESP_BASE_CONTAINER_BINDING_PROBE=ON` 并叠加 Container C3 defaults 后构建通过，镜像为 959712 字节、SHA-256 `64da754bb87ecc9e5b5d91857234f92f5c7e91668cbae584e310d61ffe4302b8`。新 WAMR 的 `SHRUNK_MEMORY` 默认值触发 Container 配置拒绝，因此 Base 探针在 `project()` 前按新组件 profile 显式设为 0；指令计量为 1，bulk/shared 为 0。Container 静态库已编译且含 `econtainer_runtime_open`、`econtainer_slots_reconcile`，但主应用 ELF 未链接这些入口，主应用无包操作调用方。此构建只验证可选组件装配，不证明 P7-02 五能力运行组合、包槽、峰值内存或实板运行。
- 独立 `IDF_TARGET=esp32` 产品配置仍在 P6-03 分区、OTA 与签名启动链守卫处明确退出；没有 ESP32 产品镜像。本轮无实板写入、生产凭据或 eFuse 修改；C3 NVS 正式预检仍阻断，以下先前条目保留各自当时的输入与结论。

2026-09-26 C3 Base 低内存与双目标前置整合候选：以 `master@2dbc24353a2eac50b6dbf5b1fa7fee8cc59d8efc` 为共同基线，合并配置缓冲/启动上下文/MQTT owner 内存收敛、C3 USB 与 ESP32 UART0 接线、目标 chip ID/OTA 收据上限，以及真实 NVS 形态只读预检。`esp_base_protocol_start` 只复制不可变元数据，保留 `esp_base_protocol_load_config` 已载入的完整配置；整结构覆盖会抹除该配置。普通 Base 的唯一依赖锁 SHA-256 仍为 `aa9e16dd65c0ee8d9f53ba89fabada2a74e6eb6b6eedccbbef50ba63fd0b575f`。

- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 核对通过；独立宿主副本的普通 C3 全量构建为 957904 字节、SHA-256 `727cbde420c661cb54fc9ff0c24c119be55bb5845b58022070d5086b6b178a0d`。生成配置为 `esp32c3`、4 MiB、USB Serial/JTAG 和自定义分区；Base host ASan/UBSan 全套、官方 NVS 生成器假件 12/12、POSIX 串口伪终端 5/5 通过。独立 `esp32` 产品构建在 P6-03 分区/OTA/签名启动链守卫处退出，未产生 ESP32 Base 镜像。
- C3 实板当前仍是 EBCF v1；两份私有完整备份的真实 `base_store` 后 31 页无法通过官方 NVS 页审计，预检保持阻断且未生成 v3 候选。ESP32 临时签名静态探针的三包槽不足现有业务包，不能冻结为产品布局。本整合分支没有刷板、没有验证 ESP32 UART 实收发、真实双槽迁移或五组件并行资源。以下原双目标及 C3 低内存条目保留各自当时的输入与镜像数值。

2026-09-26 固定 SDK C3 QEMU 的同键保页离线探针：两份以上相同的 P1-04 私有完整备份仅在仓外复制 `base_store`；独立最小项目的初始化、同键 v1→v3 提交/读回和新进程持久读取均通过。初始化 32 页不变；提交仅第 0 页变化，后 31 页逐字节不变；重启读取 v3 后 32 页无额外变化。官方 parser 仅对第 0 页加内存空白尾页的隔离观察能核对唯一活动键与转换字节，真实完整分区在前后仍因 31 页无效而拒绝；本证据不放宽正式预检、未来 NVS 写入或物理迁移条件。精确源码/SDK/QEMU 输入和边界见[离线迁移记录](base-v3-offline-migration.md#固定-sdk-qemu-同键保页探针)。

2026-09-26 P2-08 双目标软件前置：现行 C3 保留原生 USB Serial/JTAG 与自己的 4 MiB 分区表；ESP32-D0WD-V3 的控制入口改为 CH340 对应的 UART0 VFS，公开主机工具改为通用 POSIX 串口。CMake 按 IDF target 选择驱动依赖，SDK defaults 拆成共用项和目标专属项，设备事实增加 ESP32 型号。OTA 产品策略改用固定 SDK 的目标 chip ID，收据长度按目标 OTA 槽上限校验，C3 的目标、RSA v2 签名与分区数值保持原值。ESP32 的新分区布局、OTA 产品约束及 ECDSA v1 签名启动链尚未冻结，`esp32` 镜像在 CMake 顶层明确阻断；没有沿用旧 ESP-AT 或 C3 几何。唯一 `firmware/dependencies.lock` 由本次固定 SDK 重新生成，Git revision 均未改变，仅更新 MQTT component hash 与 manifest hash，锁文件 SHA-256 为 `aa9e16dd65c0ee8d9f53ba89fabada2a74e6eb6b6eedccbbef50ba63fd0b575f`。

- 仓外固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 通过 `tools/check_sdk.py`。普通 C3 全量构建成功，`esp_base.bin` 为 957696 字节（`0xe9d00`），SHA-256 `88c58e872cedf1419a730569681673bd4e9c2617084a5bfce37e9f6b1ddd887f`。生成配置确认为 `esp32c3`、4 MiB、USB Serial/JTAG、自定义分区表；官方分区解析器确认 `ota_0@0x20000/0x1e0000`、`ota_1@0x200000/0x1e0000`、`base_store@0x3e0000/0x20000`，未发生 C3 布局漂移。
- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；`python3 tools/test_preflight_v3_migration.py` 在固定 SDK 环境先 9/9 通过，加入真实 SDK NVS 页形态与无效页负例后 12/12 通过；`python3 tools/test-device-control.py` 的 POSIX 伪终端 5/5 通过。随后目标 chip ID 与 OTA 收据上限改动的 C3 增量构建及同一 host 回归再通过，最终普通镜像为 957712 字节、SHA-256 `42f3c7a3f28f9a28874505f6148c472c9765926f4618e9498630454a8c3ac485`；前一行 957696 字节只代表改动前输入。独立产品仓 `-D IDF_TARGET=esp32` 构建在预期的 P6-03 布局、OTA policy 与签名链守卫处退出；这是明确阻断，不是 ESP32 产品编译通过。P2-08 仍为软件适配进行中。
- P6-03 仓外静态容量探针使用同一固定 SDK 和本地隔离 `esp-ota@5da4a0dfbbe97723286e1a9b050e7029cff6e718`，只在仓外副本临时解除 ESP32 守卫、指定 `esp32/esp_base`、ECDSA v1 P-256 scheme 与独立 4 MiB 表。官方分区工具验证 `ota_0@0x20000/0x150000`、`ota_1@0x170000/0x150000`、`product_store@0x2c0000/0x120000`、`base_store@0x3e0000/0x20000`；未签名 Base 为 907632 字节、SHA-256 `051b9b983a60567652371ab3ad91650472be38a94e9129f4f0c9e1ed3f85e7f5`。临时 ECDSA P-256 测试键签名 Base 为 `0xffff4` 字节、SHA-256 `fd707f5d115b27638d4dd21ebdf04a65f5287acb7d4b5286c5fc123892c23d50`，`espsecure verify-signature --version 1 --keyfile` 验证有效；签名 app 相对探针槽余 `0x5000c` 字节。探针的三包槽仅各 `0x60000`（393216 字节），比现有 532480 字节签名包少 139264 字节，故此表**不成立为目标产品布局**；测试键、探针表和未公开 OTA 依赖均未写入正式清单。该证据只证明 Base 单体编译、离线签名和静态几何，不证明 ESP32 bootloader/实板可启动、产品包可用或五组件内存峰值；没有刷板。
- 私有 Tool Bridge 当前已按 Base v3 校验完整 `config.set`，并解码五项能力及两项 OTA 字节字段；其 `SerialDeviceMonitor.scan()` 仅枚举 `/dev/cu.usbmodem*`，P1-04 私有只读日志中的 ESP32 CH340 实际为 `/dev/cu.usbserial-10`，因此当前 Bridge 无法发现它。这属于 P2-08/P8-08 的独立 Tool 接线缺口，本仓通用串口 CLI 不会自动修复 Bridge。现物 C3 仍为 EBCF v1、revision 5；最新 Base 只接受 v3，不能单刷 app。只读预检及受控迁移步骤见[离线迁移](base-v3-offline-migration.md)；首次启动前还缺两槽新应用、签名启动、同键配置迁移及真实读回/恢复验证。
- P1-04 C3 两份私有完整 Flash 的真实只读预检在固定分区表、otadata 与默认 `nvs` 事实核对后**阻断**：`base_store` 第 0 页为有效 Active NVS，可独立解出唯一 v1 配置、revision 5、无 OTA 收据；后续 31 页官方 parser 均判 Invalid。32 页无全 `0xff`/全 `0x00`，摘要各异；其中四页与 `ota_1` 的现有页摘要完全相同，但不能证明所有字节来源或允许丢弃。旧 v1 源码仅把此分区作官方 NVS，固定 SDK 页管理可从第 0 页读取并把无序号异常页列为可用页，解释旧配置仍可读；正式预检没有生成候选。本轮按现物白名单补齐默认 NVS 的 SDK `phy`/Wi-Fi 记录及空 `misc` namespace，未知键、错误类型和异常页继续拒绝。是否验证保持异常页字节的原位单键转换，或在独立归因后允许离线替换整个分区，是继续迁移前的最小取舍；两条路均未获物理验证。证据及受控步骤见[离线迁移](base-v3-offline-migration.md)。

2026-09-24 `codex/c3-low-memory` 第二轮 Base 常驻 DRAM 复用：配置指纹规范编码在唯一控制任务中同步借用 NVS `s_load_bytes`；NVS 条件提交借用已结束解析的 `command.config` 作为工作值，返回前擦除。提交与读回两个独立编码缓冲及逐字节比较保持不变，20 秒候选配置、9,216 字节 USB 行、7,618 字节配置 blob 和 32 个去重槽容量也不变。相同七依赖精确锁 `f05c54cb7a1e15361b8a87b1e135ccc01ab3744490c3520de3ba583f05c582e2` 的签名五组件链接图中，`.dram0.bss` 从 `0x1a158` 降至 `0x165d8`，`_heap_start` 前移 **15,232 字节**至 `0x3fca9d60`，相对原始 Base 累计释放 **50,080 字节**初始堆空间。

- Base ASan／UBSan 全套通过，最大配置规范编码、NVS 写前／写后／commit／读回故障和工作区完整擦除均有主机断言；固定 SDK 普通 C3 构建通过，镜像 **961,568 字节**、SHA-256 `5dff4ee274ac12afa1bd1a4fbc3a3056a9f16ad0faa4ce8fccf7ba3da555027f`。仓外临时测试键签名五组件镜像 `0x121000` 字节、SHA-256 `72ca1c080823b67531207abd1a7ce19e5628e53edb99bed6a7186491440ce813`，RSA 签名验证通过。QEMU 在 Base READY 时 free／最大连续块为 **152,124／114,688 字节**；64 KiB guest 存活且完成事件时为 **61,736／40,960 字节**，两轮生命周期完成后恢复。详细生命周期、链接图和日志摘要见 [C3 小内存 Base 复测](c3-low-memory-base-probe.md)。
- 该切片没有真实 Wi-Fi、TLS、Broker、FRPS 或 OTA 下载；guest 存活时最大连续块仍不足 FRP 单次 65,552 字节 AEAD 申请。签名探针带 QEMU 专用 ADC2 空实现，不可刷实板；48 KiB free 水位通过仅指无联网仿真切片，五能力并发和 4 MiB 包槽几何仍未通过。

2026-09-24 `codex/c3-low-memory` 分支的精确依赖组合：普通 Base 的唯一 Component Manager 清单和全新解析的 `firmware/dependencies.lock` 锁定公开 `esp-mqtt@ccf81df2215cfddd87aff97afdd2e7f17e50fbaa`、`esp-frp@c56a0f32d96c75fd28e2c04146383348d8ce2829`、`esp-ota@3c3f72b823ce856b02f838fef17db1368e6d5448`，锁文件 SHA-256 为 `5825e30f7209c6fa6ddf2956e8d1706e3a081f9434af6bd392ea0d61184d61b5`。可选 Container 适配的清单锁定 `esp-container@60b65d21e4c1bf4935e791214eb5ff7174563242`，其 WAMR 仍为 `a34d721b630213f59fde0b40cebbb980903660e8`；独立探针重新解析后的七依赖锁 SHA-256 为 `f05c54cb7a1e15361b8a87b1e135ccc01ab3744490c3520de3ba583f05c582e2`，不会替换普通 Base 的五依赖锁。IDF/lwIP 固定提交未变。旧生成锁在清单变动后只更新了 manifest hash 而保留旧 Git 提交，因此本次把旧锁与 `managed_components` 移出隔离 checkout，再由清单全新解析，并逐项回读提交。

- `bash firmware/tests/run_host_tests.sh` 的 Base ASan/UBSan 全套通过；固定 SDK 的普通 C3 镜像为 961,408 字节、SHA-256 `242b47ac329e0ca94d35bbc96a8fb06a8a89ab06be0b1b8e81a2a1f580952007`。独立启用 `ESP_BASE_CONTAINER_BINDING_PROBE=ON`、叠加公开 Container C3 sdkconfig defaults 的组件编译通过，镜像为 961,392 字节、SHA-256 `5cb7912dddf9d7fcb03f3852c91ba3f62107f9d89f82dd35df9cdf84eedbbd89`。两者仍使用现有双 `0x1e0000` 应用槽且未刷板。
- 仓外测试键签名五组件 QEMU 探针以同一七依赖精确锁链接 FRP、MQTT、OTA、Container 和 WAMR；签名镜像 `0x121000` 字节、SHA-256 `26b153a46d484ba5a0d3683aabdfd7a8b56a5fb35c1255c8f369726846b6c295`，RSA 验签通过。Base READY 时 free／最大连续块为 **136,892／114,688 字节**；64 KiB guest 存活并完成事件时为 **46,504／34,816 字节**，两次 `open/init/event/stop` 均成功且 guest 返回 3。新锁下这些值与依赖升级前的静态路径切片相同；完整边界见 [C3 小内存 Base 复测](c3-low-memory-base-probe.md)。探针的 FRP、MQTT、OTA 路径仅链接，未建立 Wi-Fi、TLS、Broker、FRPS 会话或下载；QEMU 专用 ADC2 空实现和测试键使镜像不可刷实板。
- 新 MQTT 分支将运行实例常态申请缩小 4,360 字节，但三槽满时第四条在途临时消息可抵消节省；QEMU 因无 Broker 会话没有测到这项动态节省。4 MiB 的双固件／三业务包槽几何、网络动态峰值、FRP 单次 65,552 字节 AEAD 申请与 guest 并发、实板 OTA/回滚均未完成验收，不能把构建和上述 QEMU 切片当作完整五能力容量通过。

2026-09-24 `codex/c3-low-memory` 分支收敛 Base 配置缓冲、启动临时上下文和 MQTT owner 互斥缓冲的长期占用；同一五组件 QEMU 切片的 `_heap_start` 相对起点前移 34,848 字节，Base READY free 为 136,892 字节，64 KiB guest 存活时 free 为 46,504 字节、最大连续块 34,816 字节。链接图、逐阶段数值和失败路径见 [C3 小内存 Base 复测](c3-low-memory-base-probe.md)。该切片仍未证明 FRP、MQTT、OTA、Container 在真实 C3 上并发或通过内存水位。

2026-09-24 Base 当前公开依赖组合复验：普通固件的唯一 Component Manager 声明及重新生成的 `firmware/dependencies.lock` 精确解析 `esp-mqtt@5bff093646d8db810d64c50c39edc004e78bf40c`、`esp-frp@3a40a2c06580232bbe23cb981eeb21c4d14d33c1`、`esp-ota@3c3f72b823ce856b02f838fef17db1368e6d5448`，锁文件 SHA-256 为 `c170788e295f52321a132e6f5b42fdea389c180a5643528a745e6aad22342fc8`。SDK 使用公开 `esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c` 与 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d`。以下较早条目保留各自执行时的依赖与镜像事实，不代表当前锁版本。

- `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 通过。固定 SDK 的普通 ESP32-C3 构建通过，镜像 961056 字节、SHA-256 `4c23641cd7b16e95f3e055a4bfbb72585462ce7bc2a02354649a2e1d6175cc48`；仓外临时 RSA-3072 测试键的签名 ESP32-C3 构建通过，镜像 1118208 字节、SHA-256 `2e776f18d4e5c6d90e4aadb1d8f1232dd80c4895d860f3e1980855eea703789b`，`espsecure verify-signature --version 2 --keyfile` 通过。两镜像均小于 `0x1e0000` 应用槽，测试键未入库。
- 可选 Container 编译在独立副本中启用 `ESP_BASE_CONTAINER_BINDING_PROBE=ON`，精确解析含私有入口期限修复的公开 `esp-container@00c788e05d63df5279c3ca0383a778513b973601` 与其 WAMR 依赖 `a34d721b630213f59fde0b40cebbb980903660e8`，固定 SDK 的 C3 编译通过；probe 锁文件 SHA-256 为 `1237c4aa179e0d4895d92fda067071ba3a5e1ba11695e038b0ed6a1bc883687a`，镜像 961056 字节、SHA-256 为 `d136f645df42b2e4828eaaff404d20e4e62cb5b596be0f07e67c81ea53da6bd5`。该 probe 只验证可选组件装配与编译，普通 Base 锁未加入 Container，也没有证明运行时启动、执行或资源预算。
- 本轮没有刷板；真实 Broker、FRPS、HTTPS/Flash、双槽回滚、Container 包槽及五能力同板并行峰值仍需设备级验证。上述软件构建不计作对应阶段的实板验收。

2026-09-24 FRP 活动流重启回归依赖更新：Base 的唯一 Component Manager 声明与生成锁精确解析公开 `esp-frp@f31a049fe473532d59e64adf940e56511ef53652`，组件 hash 为 `8a6788d41d7b2907213f96da53454f97aa8dec16648a6f79577879957326ace6`。该提交只补官方 FRPS 的真实活动双流中断/清理/同 worker 恢复测试与中文证据，库 `src/` 未变；上游 Mbed TLS 4.1.0、ASan/UBSan 与官方 FRP v0.71.0 主机 CTest 17/17，固定 SDK/lwIP 的 C3 空输入样例链接通过。

- 本仓 `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 与固定公开 SDK 的普通 ESP32-C3 构建通过；镜像仍为 960592 字节、SHA-256 `add1d0a44378e9d857fea4de690c28c01b491087ce97ca2ee0234972490900b8`，小于 `0x1e0000` 槽。本轮没有刷板或重跑 Base 签名镜像；真实设备双流重启、MQTT/OTA 同板并行和组合资源峰值仍未验，P4-04/P4-05 不能勾验收。

2026-09-24 OTA TLS 会话票据期限修正依赖更新：Base 的唯一 Component Manager 声明与生成锁精确解析公开 `esp-ota@56944e160b1f7c919272d3a3d0c70b043e1a87d0`，组件 hash 为 `d4fa00ef861447571d4a4624075b61b92e7800021a355077c1f0cdb658d0d805`。上游在每次 TLS 读取前核对同一次读取的绝对期限，连续 TLS 1.3 会话票据不能跳过超时门；旧源码配新增回归确定性失败，修后上游主机 ASan/UBSan 与真实 HTTPS CTest 5/5、固定 SDK 普通及测试键签名 C3 构建和 RSA 验签通过。Base 的 OTA 调用合同未变。

- 本仓 `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 通过；固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 ESP32-C3 构建通过，镜像 960592 字节、SHA-256 `add1d0a44378e9d857fea4de690c28c01b491087ce97ca2ee0234972490900b8`，仍在 `0x1e0000` 槽内。普通未签名 Base 不编入 OTA 下载路径；本轮未重跑 Base 签名镜像、未刷板。真实 TLS 1.3 连续票据、设备 HTTPS/Flash、bootloader/回滚与 P5-04 墙钟验收仍缺。

2026-09-24 MQTT 重连修正与 OTA 产品归属的组合软件候选：Base 的唯一 Component Manager 清单和生成锁现分别精确消费公开 `esp-mqtt@113bdef20d862a1bf094fd3cdd833f641ab7aa64`、`esp-ota@feb6ef255d55d6e29d7904b87be92f571eb00317` 与既有 `esp-frp@9158b7f2e2c555a14636aed26b5189902152d19e`。MQTT 源仓已在真实 core/Linux 隔离 Broker 验证 clean session 断线不重放旧 SUB/UNSUB、保留 QoS1 PUBLISH 重传；Base 仍以本仓的 MQTT owner 在本次 SUBACK 后裁决 ready。OTA 库切槽前重新核对可信项目/芯片，Base 的运行/回退固件集合还逐槽核对分区几何和镜像头/app 描述；只读签名摘要不能单独作为产品授权。

- `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 通过，包含 Base MQTT owner、OTA 固件集合及可选 Container 映射的现有假件矩阵。固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 ESP32-C3 构建通过，镜像 960592 字节、SHA-256 `add1d0a44378e9d857fea4de690c28c01b491087ce97ca2ee0234972490900b8`，小于 `0x1e0000` 槽。本轮未重跑 Base 签名镜像，未刷板；真实 Broker/TLS、设备命令 ACK、双槽 bootloader/回滚、Container 包槽及完整并行峰值仍待验收。

2026-09-24 P6-10 Base 固件身份只读切片：`ota_operation` 新增 `esp_base_ota_observe_firmware_set`。在调用方独占 app/otadata 写入时，它要求运行槽与下次启动槽相同、运行槽 `VALID`，并以锁定 `esp-ota` 的 `eota_sha256_verified_image` 校验完整签名镜像和计算摘要。另一槽 `VALID` 时还要求固定 SDK `esp_ota_check_rollback_is_possible()` 为真；另一槽无可用 otadata 或标为无效时，只有其镜像校验确实失败才输出单固件集合。`NEW`、`PENDING_VERIFY`、`UNDEFINED`、状态变化和虽标无效但仍可被 bootloader 后备扫描装载的完整镜像均返回不确定、输出清零。依据为 [ESP-IDF v6.1 OTA 状态文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/ota.html)及固定 SDK `855937cf` 的 `app_update`、`bootloader_utility` 源码。返回值只是可供未来 Container 转换的固件身份事实，没有启用业务包运行。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过，新增假 SDK 矩阵覆盖双 VALID、已擦另一槽、同摘要去重、运行 pending、boot selector 不一致、另一槽状态歧义/签名镜像、回滚不可用和读态变化。固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 C3 构建通过，镜像 952032 字节、SHA-256 `8b8fe4e4cacce851465babf0d5b4a5d20a0fdd6f2e0ebf5a4e129f671e45bbab`；仓外临时 RSA-3072 测试键签名 C3 构建通过，镜像 1118208 字节、SHA-256 `c4321cc3e11aefe7be6ce7ac671f3d09e8a031cb85320ef5f16745c0c3ffa03d`，`espsecure verify-signature --version 2 --keyfile` 通过。未刷板；实际 bootloader 选择、并发 app/otadata 所有权、分区迁移与 Container 绑定仍未验收。

2026-09-24 OTA 固件集合验证接口依赖更新：Base 的唯一 Component Manager 声明与锁文件精确解析公开 `esp-ota@3731db7da35a262ff06c67951cd0e359dd1711a7`，组件 hash 为 `42796415a593ffb3118dd99906e91eeb47a9f3d16615f9b5b92c7f225a5b3bf4`。上游增加 `eota_sha256_verified_image`：对指定 OTA 应用槽执行固定 SDK 镜像校验，再按 SDK 给出的完整镜像长度计算包含签名扇区的 SHA-256；此接口不把分区存在、otadata 状态或 boot selector 当作可启动证明。该依赖更新检查点仅更新精确依赖与来源记录；其后的 Base 固件集合接线见上方记录。

- `bash firmware/tests/run_host_tests.sh` 的 Base ASan/UBSan 全套通过；固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 ESP32-C3 构建通过，镜像 951712 字节、SHA-256 `16a87f8244c6caae242fb06cc28184c9291e19e2836fdbb9d299638678f9b15d`。仓外临时 RSA-3072 测试键的签名 C3 构建通过，镜像 1118208 字节、SHA-256 `178aae6bc035ce4784e7752d2d3d68f3e2030ab8f65224ec3ceec512bd6d7061`，`espsecure verify-signature --version 2 --keyfile` 验证成功。两者均未刷板；真实 bootloader 选择、HTTPS/Flash、固件集合可启动性和设备回滚仍需实板证据。

2026-09-24 OTA 槽预检期限修正消费：Base 的唯一 Component Manager 声明和锁文件精确解析公开 `esp-ota@ffe6544401646f84a833998954e65f7250576c94`，组件 hash 为 `3eca6dbfe0e72efa2c2b6fbbc766a9d05c6b6ab6f315446b54b94c1a43c6ae8a`。上游现在从 `eota_prepare` 内槽预检前启动期限，慢预检返回后若已逾期，不再启动 HTTPS 或写 Flash；固定 ESP-IDF 的同步 Flash、TLS 和 socket 调用仍不能由库抢占，P5-04 未因此验收。Base 现有 `eota_` 调用合同未变。

- `bash firmware/tests/run_host_tests.sh` 的 Base ASan/UBSan 全套通过；固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 ESP32-C3 构建通过，镜像 951680 字节、SHA-256 `1849e65e338a915c187f83361aa273ce725d0c9e6da88c9fb1e70fe97c689c74`。这是未签名且未刷板的软件构建；本轮未运行 Base 签名构建、设备 HTTPS/Flash 或回滚。

2026-09-24 esp-ota 精确依赖更新：Base 当时的唯一 Component Manager 声明、锁文件和 README/来源记录对齐公开 `esp-ota@bed5709fe517f62d60f2efad95491bc66756a42c`。重新解析的组件 hash 为 `e875a87ce6e01f81798dd82984226861409aacec30df8770973dc058eca0c9ef`；该上游提交收紧 HTTPS 传输单次调用及 OTA 写入/哈希阶段的期限检查，Base 的 `eota_` 调用合同不变。较早检查点的 `bae8d13` 只描述当时构建，不代表当前锁版本。

- `bash firmware/tests/run_host_tests.sh` 的 Base ASan/UBSan 全套通过。固定 ESP-IDF v6.1/ESP32-C3 普通构建通过，镜像 951632 字节、SHA-256 `18ae1f2450bad05d873cc90fa722a80640ed5cf3efd3a94a3afe77413e524224`；仓外临时 RSA-3072 测试键签名构建通过，镜像 1118208 字节、SHA-256 `540a8e111e1d18c8e84f67d61314ba57c77c866ce23af2649fce546cb99ae197`，`espsecure verify-signature --version 2 --keyfile` 通过。两者均未刷板；真实 HTTPS、Flash、回滚与资源峰值不因此计为完成。

2026-09-24 P4 Base FRP 软件候选：固定公开 `esp-frp@9158b7f2e2c555a14636aed26b5189902152d19e`，普通控制任务独占 FRP 客户端生命周期。持久配置硬切为 EBCF v3：FRP 服务器域名/端口、独立 Token、显式 CA、proxy 名称、远端端口、本地端口和独立管理密钥必须完整提供；空 FRP 配置不创建客户端。owner 在 Wi-Fi IP、可信时间与已认证 loopback 管理端点均就绪后才可启动，停止与换配置必须等旧 worker 销毁完成；状态和计数可通过 USB status、周期日志及 MQTT reported 只读观察。

- 新增受限 loopback 只读 `status` 软件切片：同一控制任务先在 `127.0.0.1:local_port` 绑定，再允许 FRP owner 启动；HTTP body 的独立 HMAC、boot/单调期限、8 槽同 ID 首次快照缓存与冲突拒绝、统一脱敏结果已通过主机 ASan/UBSan，固定 SDK C3 普通构建通过。首次 FRP-only 请求仍需从本轮 USB/MQTT 获取 boot/uptime，外侧 HTTPS/FRPS 路由、同板 MQTT/OTA 并行、堆峰值和真实设备管理请求均未验。P4-05 及 FRPS/设备端到端验收保持未完成；不得把本地构建当作 FRP 可用证据。
- 离线迁移预检从两份一致的完整 Flash 备份只读提取 v1/v2 配置，生成 v3 候选且不写设备：v2 Wi-Fi/MQTT 原字段、管理密钥与 revision 逐字节保留，FRP 留空待独立正式供给。新版固件只解码 v3，不自动迁移或清 NVS。现有 v1/v2 设备未完成离线迁移、私有 Tool Bridge `config.set` 的 v3 合同未同步前，不能启动或发布此候选到设备。
- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；`IDF_PATH=<固定SDK路径> python3 tools/test_preflight_v3_migration.py` 9 项、`python3 -m unittest tools/test-device-control.py -q` 5 项通过。ESP32-C3 普通镜像 951632 字节、SHA-256 `7871386c042c16dca212cdefbc3ef57b64c3559e1839d3ddc53a84f8e78b8624`；仓外临时 RSA-3072 测试键签名镜像 1118208 字节、SHA-256 `6c5bb4783f93ee07bb1fabdf723f5cfb5c7252d41c2546213f007133568a249b`，`espsecure verify-signature --version 2 --keyfile` 验签通过。两者均在原 `0x1e0000` 应用槽内；实板 Flash、FRPS 真实证书/凭据、loopback 监听与认证、Tool Bridge、签名启动和资源峰值仍待闭合。

2026-09-24 MQTT 与 OTA 联合软件候选：以同一 Base 源码同时消费公开 `esp-mqtt@9cac455b0184420353ff0283df3f100abaac3e6b` 和 `esp-ota@bae8d13ca5f99c730c667bc55d6ea6a0d883e608`，普通控制任务保留 MQTT 命令、USB 控制、OTA 持久收据和本地 pending 自检。新隔离 checkout 解析唯一 Component Manager 锁后，`bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；固定 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd`、esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 C3 构建为 910688 字节，SHA-256 `7fc1a30e66d4adc1794837eb37c807e31196f9a7cbc4dd09490b015fafc0c441`。仓外临时 RSA-3072 测试键签名 C3 构建为 1052672 字节，SHA-256 `62b2b4e77648762386ea8f5613d1ed4411a82fcb3d582de7b134d82afabf0249`，`espsecure verify-signature --version 2` 验签通过。两镜像均未刷板；真实 Broker/HTTPS/Flash、签名基线与设备回滚仍待同板验收，不能将这次联合编译计作 P3-08 或 P5-07 总验收。

2026-09-24 普通 Base MQTT owner 软件候选：从 v2-only 配置读取独立设备凭据和管理 HMAC key；无 MQTT 配置时不创建客户端。配置存在时使用持久 UUID ClientID、严格 TLS、精确设备 Topic 和 QoS 1 retained 离线 LWT；Wi-Fi IP 与本次启动可信时间齐备才连接，command SUBACK 批准后发布 retained online 并报告 ready。进入共同控制任务前拒绝 retained、错误 Topic/QoS、超限或 HMAC 不符的命令；有效命令复用 USB decoder、身份/期限/幂等裁决，远端 config.set 仅返回 physical_usb_required。结果为 QoS 1 非 retained；每 5 秒发布一次带 boot_id/uptime/revision 的脱敏 reported。异步 OTA 终态记住原请求通道；Wi-Fi 断开与订阅失败会停会话，恢复时重新经过 SUBACK。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过，新增 owner 假运行层覆盖无凭据、TLS/UUID/LWT、SUBACK 门、已认证/retained/错 Topic 输入、结果/上报 retain 位、断连重订阅、订阅错误重试、配置更换时停止失败保留旧 handle 且清除旧管理 key。固定公开 SDK ESP32-C3 普通镜像 911792 字节、SHA-256 `eda17dd49dc07fb7a58430ecbf4858a2329123dd5fe50513d675e1d36a4551ff`；仓外临时 RSA-3072 测试键签名镜像 1052672 字节、SHA-256 `9811563a344c8c6bd787742fc42c46fb84caba71180ee8fe074974200cce4c3b`，`espsecure verify-signature --version 2` 通过。两者均在 `0x1e0000` 槽内，均未刷板。
- 设备级 Broker 精确 ACL 与账户尚未部署，私有 Tool 的网络控制端尚未联调，现有实板 v1→v2 的两槽与 NVS 离线迁移尚未执行；本软件候选没有真实设备、Broker 或网络命令 ACK 的端到端证明。retained online 与 PUBACK 不能作操作终态。

2026-09-24 P5-07 软件硬切候选：普通与受控签名 Base 均精确解析公开 `esp-ota@bae8d13ca5f99c730c667bc55d6ea6a0d883e608`；删除 Base 原 HTTPS 下载、镜像摘要、槽读写和确认/回滚实现及旧转发接口。`ota_operation` 只保留可信项目/芯片/分区/期限约束和 `base_store/base_ota/operation` 持久收据；设备协议仍拥有授权、Wi-Fi/时间前置、单 worker、配置互斥、同 ID 去重及重启调度；主程序仍拥有本地启动检查、30 秒窗口和跨窗口控制进展。成功查询通过 `eota_observe_slots` 和 `eota_sha256_running` 绑定真实新槽与完整 signed bin 摘要。

- 主机 ASan/UBSan 全套通过。Base 的 `ota_receipt_test` 覆盖收据 commit/读回、同 ID 去重与未决拒绝、NVS 不确定及 pending/VALID/回滚裁决；`ota_startup_test` 与 `control_state_test` 覆盖产品自检、30 秒窗口、写门和失败回滚。迁出的通用 SDK/HTTP/槽故障矩阵由 `esp-ota` 自有 `ota_test`、`update_test`、`http_deadline_test`、`http_transport_test` 及真实 HTTPS 回环维护。
- 固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd`、esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 ESP32-C3 构建通过，镜像 `0xc10d0` 字节；仓外临时 RSA-3072 测试键的签名 C3 构建通过，镜像 `0x101000` 字节，`espsecure verify-signature` 验签成功。两者均小于原双 `0x1e0000` 应用槽，不含生产密钥，也未刷板。
- P5-06 独立实板 HTTPS/Flash/bootloader 故障矩阵尚未完成，本候选不能计作 P5-07 总验收。当前实板是未签名旧基座；v2 NVS 离线迁移、签名首次迁移、真实 Wi-Fi/时间、槽确认和回滚仍待按恢复基线执行。固定 SDK 单次 HTTP/TLS/Flash 调用不可抢占，30 秒无进展/5 分钟总期限不代表完整 prepare 的严格墙钟上界。

2026-09-23 MQTT 公开组件硬切软件候选：移除 Base 原有通用运行层与重复 host 用例，实验应用直接消费公开 `esp-mqtt` 的固定 Git 提交。普通与实验应用的 C3 构建、Base host 回归及唯一组件/锁文件核对见[候选记录](mqtt-hard-cut-candidate.md)。原 2026-09-22 MQTT 实板证据只覆盖当时旧镜像；公开组件的独立实板矩阵及 Base 命令 ACK 闭环尚未完成，本候选未刷板。

2026-09-23 保存前软件复核：当前工作树的 `bash firmware/tests/run_host_tests.sh` 全套 ASan/UBSan 通过；固定 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 与 esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d` 在独立临时 build/sdkconfig 下完成 ESP32-C3 普通构建，镜像 786336 字节、SHA-256 `7f0e394d3795be16caddc51e4d49ebba3b47390012dff53ee79fad794bdfdb50`。同一 SDK 的受控签名构建镜像 1052672 字节、SHA-256 `cbccc852affc8d4b9025e7018df8f48c3c8f1f52d289a780ba57e1c31851de4c`；仓外临时 RSA-3072 测试键的签名经 `espsecure` 验证通过。上述两种镜像均未刷板，临时密钥不入仓，真实 HTTPS、签名槽切换与回滚仍待实板验收。

2026-09-23 P2/P5 网络故障与本地确认软件修正：普通 Base 在 Wi-Fi 驱动初始化失败时保留 USB 控制任务，报告 Wi-Fi `failed`，使 pending 槽仍可按身份、配置和控制进展完成本地确认。配置候选不能凭失败网络提交；网络失败不会单独触发固件回滚。关联/IP 证明改为按实际 SSID 长度比较，忽略驱动 AP 记录中终止符后的填充字节。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；新增真实 Wi-Fi 源码的 11 项 SDK 初始化/配置失败注入，以及不同 SSID 拒绝和 AP 记录填充字节差异的事件验证。ESP-IDF v6.1 / ESP32-C3 普通构建通过，镜像 786320 字节、SHA-256 `12c7044e602573aa2861096ca9c4186db5a0e91342ef1c6b6e6c93eb20915db4`，保持原 4 MiB/双应用槽。构建位于临时目录，未刷板、未验证真实无线故障或新槽回滚。

2026-09-23 P5 OTA operation 持久查询软件切片：签名构建新增只读 USB `ota.result`，以新查询的 request_id 和原 operation ID 读取最近一次 `base_store/base_ota/operation` 收据。`ota.start` 在启动 worker 和写目标槽前单 blob 登记设备 ID、operation ID、完整 signed bin 摘要/长度和源/目标槽，commit 后精确读回；同 operation ID 不跨 boot 重新下载。未决前次结果拒绝新 ID 覆盖，可能需要外部恢复处理；新操作只替换已有成功或失败证据的收据。当前槽 VALID、selector 一致及目标槽状态安全仍是启动前置。worker 活跃或目标 pending 返回 running；只有新槽本地自检确认 VALID 且按收据长度读回的运行镜像 SHA-256 相同才 succeeded；明确的 worker 失败、目标 ABORTED/INVALID 且旧槽有效才 failed，其余 unknown。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；新增真实收据源码 host 故障注入覆盖登记、同 ID 去重/冲突、活跃 worker 不误报失败、pending、VALID 加整镜像摘要、回滚、目标槽不安全及 NVS 写前/写后/commit/读回异常。普通 ESP32-C3 与仓外临时测试键签名 ESP32-C3 构建通过；当前普通镜像 `0xbfee0` 字节、签名镜像 `0x101000` 字节，仍在 `0x1e0000` 槽内；临时测试键的签名验证通过。未刷板、未改生产签名材料。
- 回滚目标若是没有 `ota.result` 的较旧固件，该镜像无法读取新收据；工具必须把最终结果记 unknown，不能从 USB 消失或旧槽出现推断成功/失败。host 假件不模拟真实 NVS 掉电原子性、bootloader 状态转换、设备上整镜像 SHA 耗时与 USB 负载；需要同板双槽部署和断流/回滚实测后才能给设备最终结果验收。

2026-09-23 P5 OTA HTTPS 软件链：受控签名构建增加 USB `ota.start`、独立下载任务、官方 `esp_http_client` HTTPS 与 `app_update`、完整 signed bin 的长度和 staged 槽 SHA-256、IDF RSA-3072 签名校验与 A/B 选择。下载前要求当前运行槽和 boot 槽一致且 otadata 为 VALID，目标为另一 OTA 槽，声明长度不超 `0x1e0000`；HTTPS 证书包校验、禁重定向且要求 HTTP 200/Content-Length 精确匹配。`esp_ota_end` 验签、`esp_ota_set_boot_partition` 切槽后核对 selector 和 SDK 回退可行性，异常尝试选回旧槽并读回；恢复不明时输出 `ESP_BASE_OTA_RECOVERY_REQUIRED`，不自动复位。配置候选与下载互斥，pending 自检独立于公网；签名构建的下载要求本次启动时间同步和 Wi-Fi IP。普通未签名构建拒绝 OTA。

- host ASan/UBSan 全套通过；新增实际 worker 源码的 SDK 故障注入，覆盖超槽、目标错误、响应头及首块 EAGAIN、body 断流、不完整、摘要不符、签名拒绝、selector 回退和恢复失败。pending 确认 API 返回错误但持久 otadata 已 VALID 时按真实状态清门，主程序故障测试已更新。
- ESP-IDF v6.1 / ESP32-C3 普通构建通过，镜像 `0xbee10` 字节；使用仓外临时 RSA-3072 测试键的隔离签名构建通过，签名镜像 `0x101000` 字节（含 64 KiB padding 与签名扇区），仍在 `0x1e0000` 槽内；`espsecure verify-signature --version 2` 校验通过。测试键未入库，本轮未刷板、未改 eFuse 或持久 Wi-Fi，也未连接实际 HTTPS 服务器。
- 当前实板是未签名基座，IDF 的软件验签需要运行中的已签名镜像提供公钥；首次迁移必须验证旧 bootloader rollback、备份/恢复基线、签名运行槽及 VALID otadata，并保留真实回退镜像。测试键构建不是生产签名。应用对 HTTP 取头和每次 body read 的 EAGAIN、零字节及成功返回检查 30 秒无进展/5 分钟总期限；IDF 单次调用仍可能在持续慢速滴流中反复读取，因此这些策略值不是严格墙钟上界。真实 HTTPS/TLS、网络中断、错误签名/摘要/target、完整新槽启动/本地确认/回滚、掉电与外部串口 Flash 租约尚未实板验收；跨 boot 软件回执见页首切片，尚无实板验收。

2026-09-23 P5 普通基座时间同步软件切片：新增 `time_runtime`，普通应用从编译期 `CONFIG_ESP_BASE_TIME_SERVER`（默认 `pool.ntp.org`）启动官方 `esp_netif_sntp`。控制任务每秒以零等待读取同步事件，只有本次 boot 收到事件且 Unix 时间不早于 2024-01-01，心跳才报告 `time_ready=true`；仅有一个看似合理的时钟值不算同步。Wi-Fi 断开后保留本次同步事实与运行中的系统时钟，后续 SNTP 事件会更新判断。初始化失败保持未就绪并报告错误，不触发 pending OTA 回滚或关闭 USB 控制；服务器不进入 NVS 或配置协议。

- 全套 host ASan/UBSan 通过；新增故障注入覆盖初始化失败后重试、无事件不就绪、无效同步时间、时钟回退、SDK 状态失效、非阻塞轮询和服务器字符串生命周期。pending 启动测试确认时间初始化失败不阻止本地 30 秒确认。
- ESP-IDF v6.1 / ESP32-C3 在独立临时 build/sdkconfig 中编译通过，默认时间服务器配置生效，普通镜像 779280 字节，仍在原 `0x1e0000` 应用槽内。本轮没有刷板、修改持久 Wi-Fi、连接 NTP 服务器或生成正式发布制品。
- SNTP 成功事件与日历下界只证明本机已按 SDK 路径同步，不提供加密时间认证；真实网络同步、Wi-Fi 重连后的再次校时和故障恢复仍待实板验收。HTTPS OTA 的后续软件接入见页首记录。

2026-09-23 P5 OTA pending 本地确认门：普通基座先读取运行槽，再完成 NVS、身份、已提交配置和 USB/Wi-Fi 控制任务启动检查。仅对 pending 槽要求控制循环 5 秒内完成首轮，在同一启动 30 秒窗口中每秒核对最近进展不超过 5 秒，窗口终点之后最多再等 5 秒取得新一轮进展，再调用 IDF `esp_ota_mark_app_valid_cancel_rollback()`。pending 期间 `config.set` 返回 `ota_verification_pending`，确认成功后恢复；Wi-Fi、Broker 与 FRPS 在线不是确认条件。启动/活性失败调用 IDF 无效回滚重启；无可回退镜像时不强制重启，日志输出 `ESP_BASE_OTA_RECOVERY_REQUIRED`。确认 SDK 返回错误后读回 otadata，按真实 pending/valid/未知状态裁决，不假定失败原子。构建时强制开启 bootloader rollback。

- `bash firmware/tests/run_host_tests.sh` 的 ASan/UBSan 全套通过；OTA 假件覆盖 NVS/身份/配置/控制任务失败、控制循环首轮与中途失活、第 29 秒后退出、跨窗口新进展、5 秒活性边界、30 秒窗口、pending 配置写门及确认后恢复、无回退镜像、SDK 返回错误但持久状态已 valid 等情形。假件不模拟真实 bootloader、Flash 掉电、并发任务或 USB/配置提交最长阻塞时间。
- ESP-IDF v6.1 / ESP32-C3 的普通应用在独立临时 build/sdkconfig 中编译通过，镜像 768480 字节，SHA-256 `c630119149bb65f4b00e9bbb6e66d56e271d5773e613fb1f5aa1741d3c254907`，双 `0x1e0000` 应用槽不变。本轮没有刷写设备或创建正式发布制品。
- 5 秒是当前控制循环的活性策略值，合法长阻塞与真实负载尚待实板验证。此子项当时只关闭启动后的本地确认和失败回滚接线；HTTPS 下载、目标/摘要/签名校验的后续软件接入见页首记录。真实新槽启动、无回退镜像的人工恢复路径、与配置/网络能力并行的实板验收仍待实施。维护者暂缓的人工断电用例不计通过。

2026-09-22 后续 P2 实板验收：人工在 RAM 候选配置验证期间断开 USB，收到 running 后的请求第 12.171 秒观察到设备消失，7.482 秒后重新接通。新 boot 报告 power_on，原 UUID、revision 5 与 Wi-Fi 恢复；断电前后各两份完整 Flash 全部逐字节一致，原 Mac Bridge 已恢复。本轮未刷写固件，首次发送前串口 ENXIO 的尝试保留且不计通过；详见[配置候选断电验收](config-power-loss-acceptance.md)。Flash 实际写入中间态掉电仍未完成。

同日后续补齐动态单项订阅、退订与重连期望列表，订阅提交失败改为停止会话。新 TLS 实板完成 100 次逐轮销毁/在线资源采样和三次 station 恢复；显式 TCP 实验通过载荷、动态订阅、一次 station 恢复及五次重建。任务/socket 分别稳定为销毁后 7/0、在线 8/1，所有当前任务栈余量至少 1196 字节。两轮完整 Flash 恢复、UUID、revision 5、Wi-Fi 与原 Bridge 已验证；详见下方验收记录。外部 AP/WPA3、提交中间态断电及五能力组合仍未完成。

2026-09-22 P3 实板网络检查点：同一 C3 完成隔离 TLS Broker 的十项 QoS/载荷往返、100 次完整 MQTT 客户端重建、Broker/认证/证书故障恢复、超限拒绝、retained/LWT、丢 ACK 重传、outbox 满/过期及 SUBACK 拒绝。原固件、完整 Flash、UUID、revision 5、Wi-Fi 与 Mac Bridge 已恢复并验证。详细制品、矩阵、资源范围和失败尝试见 [MQTT 实板验收记录](mqtt-hardware-acceptance.md)。该首轮尚未覆盖的动态订阅、TCP 和逐轮观测已在上述后续检查补齐；普通基座仍未接入 MQTT 设备控制。

以下为同日较早的软件检查点，“未刷写”只描述该时点：

2026-09-22 P3 软件接入检查点：新增 mqtt_runtime 与独立 mqtt_integration 实验应用，锁定官方 `espressif/mqtt == 1.1.0`，没有引入自研 MQTT 报文、QoS 或 outbox。普通基座仍报告 MQTT unsupported；MQTT 尚未接入持久配置、设备命令和 reported。

- 配置、主题与 QoS 边界、4 KiB 有界分片重组、精确 SUBACK 确认、动态订阅/退订、单 owner 生命周期、16 项通知与 3 项完整消息槽已经实现。队列丢失或订阅证明失败会停止会话并明确失败，不能继续声称业务在线。
- SDK 配置检查发现默认未开启证书日期验证；defaults 与编译约束现要求 `CONFIG_MBEDTLS_HAVE_TIME_DATE=y`。TLS 还要求 owner 已同步时间、显式 CA 和主机名校验，实验应用通过官方 SNTP 取得时间。普通应用已实测拒绝实验输入和明文实验选项；正式发布门禁仍待后续实现。
- ASan/UBSan host 回归通过，包含 10000 次分片字段畸变、SDK 初始化/停止失败、outbox 满/过期、错误认证/TLS 分类、订阅拒绝/超时、队列溢出，以及 100 次模拟 SDK 的创建和释放。这些是装配层与事件注入结果，不是板卡/Broker 或真实并发验收。
- ESP-IDF v6.1 / C3 两应用构建通过：普通基座 760576 字节，SHA-256 `a68d33373b2b42a79daa51967d4f85e5a35cbff2fdd840ea4a2debb0aa478b86`；实验应用 884032 字节，SHA-256 `e2f14370768fb3aeacd245dd2f95b6da44c9a91dbe5f1705b4f137c3c6cf2fef`，保留 LAB_ONLY 标记。两者均使用原 4 MiB / 双槽分区。实验输入只是不可连接的示例 fixture，没有真实凭据。
- 构建 receipt、SDK/compiler/CMake/Ninja 版本、源码摘要、依赖锁、sdkconfig、分区、二进制与日志保存在 ESP Tool 私有忽略目录 `provisioning/receipts/private/p3-mqtt-20260922/`。本轮没有刷写设备、连接 Broker 或验证 MQTT 的真实堆峰值。P2 提交期间掉电验收、P3 实板矩阵、正常基座组合与后续阶段继续待完成。

2026-09-22 P2 配置/Wi-Fi 检查点：在同一 C3 上完成 config.set、独立 base_store 分区的完整配置条件提交、候选验证、Wi-Fi RAM 配置与退避重连。当前开发镜像为 760576 字节，SHA-256 `5097fb549ff792680a311144b44340f5470a78bcd11b3694d29e77d07d684773`。

- 错误 SSID、错误口令均未增加 revision，原已提交配置重新连接成功；正确配置、重复请求原结果、同 ID 不同内容冲突、旧 revision 拒绝、显式清除与重新配置均通过。
- 软件重启后同 UUID、新 boot、原 revision 和 Wi-Fi 连接恢复通过。Wi-Fi 在线时 100 次 USB 查询无重启，free heap 范围 211180–211216 字节；这不是五能力峰值或长稳测量。
- Bridge HTTP 配置成功校验新 revision，20 秒失败候选返回明确失败并保留原配置；公开 CLI config.set 通过。最终 revision 为 5。
- 两次完整 Flash 读回摘要一致，核对身份 UUID、NVS header/data CRC、blob 当前索引和完整配置字节；revision 与 Wi-Fi 凭据同 blob 保存，启动区、分区、OTA selector 未改。最终恢复件仅保存在 ESP Tool 私有忽略目录。
- 人工拔除 USB 电源后重新接通，确认真实 power_on、新 boot、同 UUID、revision 5 与 Wi-Fi 自动恢复。首次尝试暴露主机串口库打开端口会额外复位，修复为 POSIX 直接打开并禁用关闭挂断后复测通过；公开 CLI 连续打开三次保持同 boot，伪终端字节传输与背压期限测试通过。
- ASan/UBSan 覆盖命令解析、配置编解码、revision 冲突/耗尽和 SDK 存储调用的故障注入；ESP-IDF 编译通过。上述较早断电发生在配置已提交后；RAM 候选期间断电已在本页后续记录中补齐。Flash 写入中间态掉电、WPA3 和物理插拔完整矩阵尚未验收，P2 保持实施中。

以下 USB 检查点对应本轮较早镜像，保留其范围明确的验收事实。

2026-09-22 P2 USB 检查点：持久身份、每次启动 UUID、严格 JSON Lines 解析、命令 guard、状态结果及重启回执已接入真实 USB。ESP-IDF v6.1 / ESP32-C3 编译与 ASan/UBSan host 回归通过；cJSON 固定为官方 `1.7.19~2`，提交依赖锁。

- 在迁移基线对应的同一块 4 MiB C3 上重新核对 USB、NVS 身份和有效 OTA 选择，取得两份一致的完整 Flash 恢复件；只写入既有 `ota_0` 应用。
- 验证错误设备、旧 boot、过期、超长期限、未知命令、错误参数、重复字段、错误类型、非法 UTF-8、8193 字节超限、分片和半帧超时恢复。连续 100 次状态查询没有重启，free heap 始终为 322036 字节。
- 实板超长输入暴露官方缓冲驱动 RX ring 满后丢数据的问题，改用官方无缓冲 VFS 和 FIFO 背压后整组通过。公开 Python 示例的 status/restart、私有 Bridge 的 HTTP→USB 状态/重启/重复请求/旧 boot 拒绝均通过。
- 写后读回确认启动区、分区表、身份 NVS、OTA selector 和 `base_store` 逐字节未变，应用读回摘要一致；没有修改 eFuse、分区布局或 GPIO。

本轮应用为 160800 字节，SHA-256 `9bfbb1ab10b6dbd0dabed4286c40ebcf075d79bc65422b63ae7409b43d982456`。真实身份、原始日志和恢复字节只进入 ESP Tool 私有忽略目录，不进入公开仓。上述短时 USB 测试不证明配置/Wi-Fi、MQTT、FRP、OTA 或 72 小时长稳；P2 尚未完成。

以下为 2026-09-21 历史基线，当时尚未刷写实板。

2026-09-21 开发基线：ESP-IDF v6.1 / ESP32-C3 构建通过；命令身份、boot、期限、去重与容量 host 回归通过 ASan/UBSan。当前设备协议仍是迁入心跳；命令 guard 尚未接入实际执行，配置/Wi-Fi/MQTT/FRP/OTA 继续实施。未刷写实板。

开发基线已保存至 `master` 的 `16a903e15d2fea2fe5c09a9aa17f622c5821da21`。已从 GitHub 重新 clone 到工作区之外的全新目录进行独立验证，不复用原 checkout 的源码或构建产物。 ESP-IDF v6.1 / ESP32-C3 编译及 ASan/UBSan 命令裁决测试通过；应用大小 0x25120（151840）字节，单槽 0x1e0000，剩余约 92%。

正式发布与完整硬件验收仍未完成。旧独立 checkout 结论只覆盖上述历史 commit，不覆盖后续 USB、配置与 MQTT 改动；当前保存状态以实际 HEAD 与工作树为准。
