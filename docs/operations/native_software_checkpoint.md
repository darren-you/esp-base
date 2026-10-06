# 原生业务与独立固件 OTA 软件检查点

日期：2026-10-06。对应[唯一执行计划](ota-allocation-diagnostic-checkpoint.md)的 R1–R4 软件实施。维护者明确“先完成软件，实板稍后接入”；本轮只执行源码、离线构建和宿主回归，没有连接设备、刷写、修改 eFuse、部署生产或删除仓库。R3 的真实 FRPS／公网路线、R5 双板容量与断电、R6 每板百次和连续 72 小时、R7 实际删仓与正式交付保持未完成。

同日职责调整初次记录：ESP Tool Mac App 只做本机 ESP 管理，删除 App 内置 FRP／frpc 与远程 Bridge。当时只调整文档并回写R3待办；此前涉及Bridge的软件与测试结果仍是范围调整前真实记录。后继删除与Rust精确消费的软件结果见文末各节，旧阶段数字不赋予新阶段资格。设备自身FRP／MQTT和Server／Web直接面向设备的远程职责保持。

## 已实现的软件合同

Base 的原生业务由既有控制 owner 同步执行认证 MQTT 二进制事件，保留计数、暂停／恢复、状态查询、非延期 100 ms 窗口与重启清零语义。输入不再携带业务包摘要；完整 MQTT 帧仍最多 4096 B，原生业务输入最多 3924 B。没有新增业务线程、队列或输入副本。报送明确区分认证接受、业务执行结果与 Broker PUBACK。

生产图删除 Container binding、guest、产品 worker／队列、动态业务包来源与账本、联合 OTA 包模式及 ECS2。专属构包、生命周期和 NVS 容量探针入口一并退出，历史证据通过已核验的 Git 永久链接保留。通用 NVS 同 key 探针、官方 SDK、FRP、MQTT 与固件 OTA 仍归各自真实仓库。

固件 OTA 使用 182 B V4 收据，包含设备、原 operation、精确来源／候选签名身份、槽、尺寸与失败结果。意图写入并读回后才启动唯一 worker；长升级 claim 和短 Flash I/O claim 分开，上传期间既有网络 owner 仍工作。准备结果不等于持久终态，成功须在新 boot 的本地控制进展连续 30 秒后确认；该确认不要求 Broker 或 FRPS 在线。旧 V3、未决、损坏与来源不确定均阻断普通升级，不自动当成空状态。

设备 FRP 新增固件／业务查询、暂停／恢复、升级提交与原 ID 查询。认证 PUT 绑定已持久意图、三 UUID、完整 signed bin 摘要和尺寸，最多 1024 B 预读移交；同一监听器继续接收查询，重复上传和第二操作拒绝。连接／单次 I/O／无进展／传输总期限为 5／1／30／300 秒。HTTPS URL 来源与入站流复用精确 esp-ota 的签名、目标、摘要、几何、备用槽写入及选择机制。协议细节见[设备协议](../design/device-protocol.md)和[FRP 软件检查点](frp_ota_software_checkpoint.md)。

最后审阅发现并修复上传终态顺序：worker 结束上传时，后继控制 owner 可能尚未持久化失败。PUT 现在只返回 `running/202` 或 `unknown/200`，不提前报告 `failed`；客户端只通过原 operation 查询确认终态。自然顺序测试证明上传回复时失败提交次数仍为零，持久提交／读回成功后才发布失败并释放 claim；存储不确定保留锁和 unknown。

此前 Tool 的 Go／Web／Mac Bridge 已消费原生业务与固件独立身份，当时保留绑定、授权、USB 租约和原 ID 对账；这是范围调整前的软件事实。设备独立 FRP 不要求 USB 或 Mac 在线，本机 USB OTA 下发 URL 后仍由设备 HTTPS 拉取镜像，不能据此获得全 USB 固件传输资格。当前 Mac App 只保留本机设备能力，远程 Bridge 及其绑定／授权接线待删除，USB 租约与原 ID 对账继续保留。独立 SDK 的新有线 Flash 能力由 Tool 后续事项跟踪，现有软件替身不构成真实有线刷写／恢复通过。

该独立有线事项随后完成SDK540原生ROM／Flash与App刷写／原ID恢复软件：SDK29项、App111通过／113项（2实板条件跳过）、双架构本地无Python包门通过。374冻结源码及实际包／日志已独立回读；来源、旧数字和实板／商店边界见[Tool补验](../../../esp-tool/docs/operations/native_software_checkpoint.md#mac-有线-flash-与原-id-恢复软件补验)。本次只同步软件事实，不消费其SDK gitlink或源码保存范围，也不授予R3实际USB资格。

Fast Deploy Core／Panel 退役动态业务包专属读取能力、路由、配置与响应例外，通用 retained 制品机制和历史对象保持。Nginx 删除三个专属产品包 location；Bruno 从稳定 OpenAPI 重生成 Tool、Bridge 与 Panel 三集合。当前生产仍可能运行旧发布版本，源码退役不等于生产入口已退出。本轮不升级产品 Core／SDK gitlink，不变更有效凭据。 Tool完整软件结果见[Tool检查点](../../../esp-tool/docs/operations/native_software_checkpoint.md)：Go19个测试包、132项顶层／617含子项通过，Swift44／46通过（2实体串口跳过），73脚本／43浏览器通过；唯一Go独立Swift listener跳过已由Swift调用真实Go race补验，25项MQTT／Router真实TLS Broker补验已包含最终Go数量，不重复累加。

## 双目标布局与首次迁入

| 区域 | ESP32-C3 | ESP32 |
| --- | --- | --- |
| ota_0 | 0x20000／0x1e0000 | 0x20000／0x1e0000 |
| ota_1 | 0x200000／0x1e0000 | 0x200000／0x1e0000 |
| otadata | 0xf000／0x2000 | 0x10000／0x2000 |
| FRP scratch | 0x3e5000／0x10000 | 0x3ea000／0x10000 |
| base_store | 0x3f5000／0xb000，11 页 | 0x3fa000／0x6000，6 页 |
| 旧 AT 原始区 | 不适用 | 0x3e6000／0x4000，只读 |

来源为[C3](../../firmware/partitions/c3-partition-table.csv)与[ESP32](../../firmware/partitions/esp32-partition-table.csv)真实 CSV；已删除 product_pkgs，完整 Flash 为 4194304 B。普通 app OTA 不能改变分区或 bootloader，新布局必须首次有线迁入。

`tools/prepare_native_layout.py` 只生成离线候选，要求两份字节相同、权限受控的新鲜完整备份，核对旧分区、设备身份、CRC、真实签名 A／C、V3 与 ECS2 终态。未决／PREPARED／unknown／损坏阻断。先归档并读回原完整 Flash、旧收据和原 operation，再生成新官方分区、双签名 app 与 VALID otadata；UUID、配置 revision、凭据及其他活动 NVS 记录按精确原字节保留。旧已证实终态退出运行账本，迁入后旧 operation 由私有迁入收据查询，设备如实返回 unknown；不能伪造一条属于新镜像的旧成功记录。

旧 C3 v1 配置转换和旧 ESP32 AT 首次迁入使用现有只读预检／归档器；AT 原始区按 eFuse MAC 的明确输入契约核对。官方分区 MD5 标准记录也需验证。候选标记 `software_only`，输出为私有 `source_flash.bin`、`migration_receipt.json`、`candidate_flash.bin` 及必要的 `at_old_raw.bin`。没有串口、设备选择、刷写或自动执行入口。真实迁入前仍须重新读取双新鲜完整备份，不能复用本轮测试 fixture。

## 构建与输入对应

SDK 唯一依据为 [sdk-lock.json](../../sdk-lock.json)：IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。SDK 根的 lwIP gitlink 差异正是该显式 pin，检查器拒绝任何其他修改，不把官方 CMake 的 submodule 警告隐藏。

两份官方 Component Manager 锁冻结 OTA `8ab62f98fba2ea8e76c2822d0e7bf1cb523088a1`、FRP `989cc876d92b815aeb0b6806fb861f0ee2b39a86`、MQTT `6443b71db761f4d667503f14108687bad5e6b5ee` 与 cJSON `1.7.19~2`。未手工伪造组件摘要。两份仓外独立源码副本、独立 sdkconfig／build 和测试签名键执行全量构建；上传顺序修复后再次构建并验签，原先制品不冒充最终制品。C3 RSA v2、ESP32 ECDSA v1，signed boot/update 与 rollback 的真实配置分别核对，软件键不属于正式生产信任。

上传终态顺序修复后的冻结软件制品（补审短Flash竞争修复之前，当前修复构建见文末）：

| 目标 | 完整 signed bin／B | 每槽剩余／B | signed bin SHA-256 |
| --- | ---: | ---: | --- |
| esp32c3 | 1052672 | 913408 | `71a4d6ab1d5fd4b6c03828292fce79f3e93660363b4b99ab7ee2df391518b823` |
| esp32 | 983028 | 983052 | `a0cf903d1ef6462d3875612cc9013af2ac08582037595f0df688975478a0ba4e` |

两目标每槽均为 1966080 B。官方 espsecure 5.4.0 分别验签通过，ESP32 分区表 ECDSA v1 也通过；无需改变硬件安全状态或 eFuse。

71 个生产输入集合 SHA-256 为 `6ea12a56b8023cc053986f9cff89d15a10eb7ba3b17aa10c58be7962a57b555e`；两目标实际编译源码分别 942／952 个，逐项摘要已保存。完整私有软件实体保存在 `receipts/private/native_software_20261006`，共 2128 个已读回核验成员，manifest SHA-256 `85621fea56791d546405f27c9d7baa0f697aa1e6ce4071948dbe2e84f75455ac`。包括生产与测试源快照、精确 managed components、ELF／bin／map、sdkconfig、编译源与命令、分区、软件公钥、大小报告和前后日志；没有本轮固件签名私钥或生产凭据，原样 managed 源自带的公开第三方 example／test 键是源码 fixture。该目录已由既有 Git ignore 规则隔离，文件0600、需要执行的源文件仅增加owner执行位。

软件公钥 SPKI SHA-256：

- esp32c3：`df5b34eefa8a9ee19d8881acb6233b05703f7e9f001a03686d6273e0a2012036`。
- esp32：`58dbfe5eaa31443bd8689e48e1a36068b00cd4d8bca79c394cc0034e2f17819d`。

最终链接布局的 DRAM used 为 C3 95604 B、ESP32 42210 B，ESP32 IRAM used 96315 B；RTC SLOW used 为 7700／7682 B。这些是官方 size 静态布局，未占用量不能直接当作运行时可用 heap、连续块或容量通过。

## 静态资源盘点

实际 MCU ELF／DWARF 的两目标类型与符号均核对：原生业务 state 64 B，上传 state 1248 B（含 1024 B 预读），listener input 1537 B、output 1024 B，protocol response 1024 B，MQTT event 4388 B，原请求 guard 2568 B、结果槽 2048 B。C3 live config 7640 B 位于 RTC，ESP32 同型 config 位于 8BIT IRAM。MCU 的上传 state 不能用 host 的 1256 B 替代。

已识别运行申请：USB line 最多 9217 B，扩容复制时与旧 8192 B 缓冲短暂重叠，reader 为 16 B；OTA 请求为 592 B，配置候选 7608 B，MQTT 配置工作区 7716 B。cJSON DOM、SDK/TLS、队列/outbox及验签的实际能力域与峰值仍需联合实板测量，不能由这些类型大小推定总最大申请。

任务栈保留 main 6144 B、control 8192 B、临时 OTA 12288 B、MQTT 6144 B、FRP 6144 B；SDK Wi-Fi、lwIP、timer／event、idle等任务也属于 R5 全任务验收，不仅检查上述自有任务。固定 stack reservation 不等于实际剩余水位。

TLS 仍为 16 KiB 入／4 KiB 出与动态缓冲，Wi-Fi 6 个静态 RX／BA、32 个动态 RX／TX，MQTT 4096 B frame／5120 B publish／16384 B outbox，FRP 双活跃加备用流／65536 B 控制记录、12 sockets 保持。流准备保留 64 B read 与 1024 B write 工作区，不复制整镜像。真实 MCU ELF 没有 Container／WAMR／产品账本运行符号，真实编译图没有相关源码；源码注释和历史记录不作为链接残留。

Flash 总体成本仍包括 app 擦写、otadata、失败／成功收据、配置提交与 FRP scratch。5 年和每小时最多一次 >4 KiB 加密控制记录是假设，需另计启动、重试与热点扇区；普通固件 TCP 数据不按 scratch 控制记录计算。本轮没有实测擦写最坏时延或确认具体 Flash 规格。

## 验证与未取得的资格

两目标严格编译、ASan／UBSan host 全部通过，覆盖原生业务、认证 MQTT、真实 decoder／guard、OTA owner、V4、来源与候选签名观察、启动恢复和 scratch 开／关；两目标真实 HTTP／OpenSSL HMAC／Base handler 回环通过。测试的 NVS／SDK 故障注入不代表真实断电原子性或 bootloader 通过。

迁入 8 项集成使用真实 RSA v2／ECDSA v1、官方分区与 NVS 生成器，覆盖新候选、原身份／配置／AT 保留、未决阻断、损坏／错目标／错签名／双备份不符、私有 key 输入稳定快照及归档读回失败后不发布候选。最终所有宿主工具回归 78 项通过，包括 AT 归档器12项、官方分区2项、迁入8项、设备控制、业务事件认证和 FRP CLI16项。FRP CLI 和 Tool 的运输 fixture 未冒充正式签名固件或实体 FRPS。

Core retained 16 项、完整制品发布/finalize/故障恢复自测与 Profile 渲染通过；Panel 全仓 race／vet／build、统一 REST 响应检查通过。中央响应例外回归 65 项通过，退役路径不得再声明二进制响应豁免。Nginx 两目标本地路由／语法、14 项真实 Gate HTTP及6项固件交付 Header 透传通过；31／5 项已有告警保留。Bruno 三集合共36请求重生成与一致性检查通过。

构建过程中曾遇环境变量作用域错误、ESP32 自定义旧 lock 未退出而解析到旧 OTA API、初次 host 组件目录漏 firmware 层、旧测试按8条/旧产品交付枚举断言失败；均按真实环境、官方 lock 解析或退役合同修正并保留前后日志。上传终态顺序缺口另加自然行为回归，不把早期构建替换为成功记录。

R5 的 16384 B 历史 internal 8BIT heap、24576 B 连续块与每任务1024 B剩余栈门未实测；公网脱离 Mac、双板完整 OTA／持久结果、全合法峰值、历史 FRP 超时回归和断电尚待。R6 每板100次／连续72小时与Flash寿命未做。R7 仓库实际删除和正式交付仅在 R1–R6 合格后执行。

## 实板接入前的软件验证准备

本节为短Flash竞争修复前的软件准备版本，保持上述71个生产输入、两份final signed制品与2128成员原归档不变；当前修复制品及观察A/C见文末补审段，旧制品不得用于当前候选R5／R6。官方FRPS宿主组合现已执行两目标成功／写入失败／NVS不确定六场景，6/6通过；真实efrp／eota、Base listener／handler／owner／V4与原ID客户端经过官方回环代理，最大两个工作流、每操作一个worker、零重复写。精确输入、官方服务端版本、签名验证、30秒本地确认及所有POSIX／Flash／SDK替身见[FRP联调检查点](frp_ota_software_checkpoint.md#官方-frps-与原生-ota-宿主联调检查点)。这项补验没有授予R3真实公网或R5实板资格。

[容量观察生成器](../../tools/prepare_capacity_observer.py)在逐项核对的仓外私有副本生成revision 2实验版。既有control每5秒采样internal 8BIT heap与最多32个完整真实任务，官方pre-deletion hook在正常清理前复制真实编号、名称和最低栈；启动早期、FRP、MQTT、SDK短任务按实际清理记录，另保留OTA完成前记录。固定64条退出缓存，每次control最多输出64条，sticky溢出使parser拒绝本轮；无新增任务／队列／堆申请、heap hook或SDK修改。主机8项覆盖BSS早期记录、名称复用、溢出、并发160条与有限flush，在双target ASan／UBSan下通过；parser另验跨日志残帧不能拼成完整快照。

两目标使用相同锁定SDK、既有软件测试键完成独立全量签名构建，官方应用验签与ESP32分区验签通过。配置仅开启官方trace／task pre-deletion hook及其隐含static cleanup hook，版本`0.2.0-capacity-lab`；关闭显式CMake实验开关拒绝配置。正式发布门尚未接入，LAB_ONLY标记不等于平台已拒绝制品。

| 目标 | 实验signed大小／B | 实验signed SHA-256 | 静态DRAM used／B | 相对生产增加／B |
| --- | ---: | --- | ---: | ---: |
| esp32c3 | 1052672 | `656f678a8baf98ad459ae5621c729112c1908871bf438902037e004b7e5b120e` | 100220 | 4616 |
| esp32 | 983028 | `e364c90b42d77604d0007bd4bd0d5e1a598ac0d0eb15da82e007fb5241d5624f` | 46826 | 4616 |

观察工作区`sizeof`为4253 B；链接自有静态符号为C3 4245 B、ESP32 4253 B，C3单核无效mux被编译器省去。整体静态差额还含trace／SDK布局，不加回任何成本。编译源943／953项分别保存摘要，实验镜像不继承冻结生产镜像资格。异常重启、panic、尚未cleanup、日志丢失和非任务栈仍可能漏观察；5秒largest采样不能证明全域历史最低连续块。锁定SDK allocator hooks可能在ISR／cache-disabled执行，free不提供size、realloc／直接路径不完整，遍历heap不能安全放进该回调；本轮不采用这条不完整证据链，全合法峰值和各能力域仍属R5外部原件。

同源实验A/C已另做独立全量构建和官方验签。上表作为A，C的完整摘要为C3 `170e09cd69a8c338fe9dc71fd096ac1a33d3a888fec6eecd367eed19f115758d`、ESP32 `03633666dae5702c507b8a240f34a475b82833b376cd562b3c3eadd615544d4b`，大小及静态布局相同。生产／观察源码、精确managed依赖和有效sdkconfig逐项相同；编译器实际生成的证书／签名公钥汇编只在路径注释不同，内嵌原字节也核对相同，原始差异与摘要仍保留。官方构建时间／ELF身份元数据使完整镜像不同，没有手改signed字节。pair收据SHA为`3cf4c7394ef0b0568e7eca08e1c1eee71c7060b46cccb186236f670eccec8108`；两份均为LAB／软件测试信任，分别待R5，不冒充生产pair。

[有限生命周期驱动](../../tools/native_lifecycle_run.py)实现固定100周期与本轮连续72小时，参数见[宿主工具](../../tools/README.md#百次与连续72小时有限驱动)。实际OTA拒绝同镜像，要求同一已评审原生实现的两份不同signed A/C、同target且均由外部R5覆盖；FRPS夹具的旧A只提供Flash初始身份，不是原生pair。驱动绑定初始device／boot、现有FRP／严格TLS MQTT信任、外部R5摘要和活动容量日志前缀，每周期一次提交，按新boot原ID、完整镜像和实际最大业务reported对账。上传期间有限业务线程的待决时间与host上传progress区间相交只表达宿主观察重叠，不能推断业务实际在正文写入期间完成或满合法峰值。

暂停／恢复、宿主会话关闭和重启后网络重建只授有限功能观察，不能代替MCU全任务释放或资源回收。普通进展、系统休眠／时钟异常与OTA有界窗口分别记录，未知／异常保留原ID并停止；原ID只读结果另记且不覆盖中断，不自动重发或换ID。即使观察完成，summary保持`qualified=false`／`r6_passed=false`，须与实板资源／公网／断电／Flash证据共同裁决。本轮没有运行真实100次或72小时，也未用旧轮次拼接时长。

Paho环境另做7项真实TLS补验：固定2.1.0、VERSION2回调、严格CA／主机名、QoS1非retained最大4096 B帧、精确reported、错boot／序号／摘要拒绝、失联且不自动重连。本机Broker显示2.1.2，但既往完整制品来源未复核，收据明确`official_release_artifact_qualification=false`；其作用只验证真实客户端API／TLS。MCU消费者为明确HostFakeMCU，Wi-Fi／FRP等字段是假输入，不授设备资源资格。临时Broker与客户端线程实际退出，现有生产凭据和服务未消费。网络测试保留当时源码快照；后续Journal互斥只改日志事务，客户端片段再核对。

本轮工具全套初次104项为103通过、1项测试环境失败：私有日志使用umask077，使fixture预设的公开0755父目录实际成为0700。明确chmod0755后，迁入8项在同样umask下补验全部通过，包括归档读回损坏后不发布候选；失败日志保留。驱动在后续修正墙钟／单调钟、命令尝试边界、跨boot原ID实际响应以及Journal真实双线程写入互斥后，29/29独立回归通过；这些受影响测试单独补验，未重复无变更套件。最终driver源码SHA为`379f736a1735d7078585b7fec259cc812a6b4707815502f9cf342e62ea6f1809`，测试SHA为`d008c29c19eef60f29df5efd1608e295059a0cfe6f6f0b4e8059525067daa057`。

追加证据保存于`receipts/private/native_validation_preparation_20261006`，1924个成员逐一回读摘要与私有权限，manifest SHA为`a1bcbfbde374ae66507acd8bc5bc378d4d0fb911d3528bcaed969d1ad7519713`；Git ignore隔离。包含官方FRPS两目标完整实体、LAB前后源码／ELF／bin／map／配置／编译输入与成本、同源A/C、原始失败及补验日志、固定Paho wheel／实际TLS响应／退出和最终源码绑定。归档初次因重复SDK输入目标被独占创建规则拒绝，部分副本另留`native_validation_preparation_20261006_incomplete_1`并明确未完成；修正输入清单后重新独立归档，不覆写旧证据。原2128成员、manifest和71个生产输入在前后再次逐项核对不变。

当前R3真实公网／USB、R5双板容量与断电、R6双板长稳与Flash、R7删仓和正式交付均未勾选。

## 短 Flash 交接与宿主连接期限补审

补审对真实main回调复现TSan数据竞争：OTA worker释放共享`claim.token`时，控制任务在BUSY重试中读取同一非atomic字段。此前官方FRPS假件另加mutex，不能证明生产回调正确。当前等待者改用局部claim，取得真实owner后才写共享交接；释放先清共享交接，再以局部claim发布owner空闲。原owner CAS的release／acquire建立交接顺序，没有新增锁、线程或常驻工作区，500ms BUSY预算保持。

新测试直接include生产main回调，两目标完整host ASan／UBSan及独立TSan均通过，包含8192次双线程竞争、顺序交接、BUSY超期和原claim保持。原数据竞争的源码、TSan失败和当前日志分别保留。它们不证明MCU调度、实测Flash时延或栈水位。

宿主旧`socket.create_connection`会把同一5秒预算重新交给每个地址，确定性测试在两地址复现10秒，后继同项回归的旧源码为9秒失败。当前DNS、逐地址连接和TLS共用绝对5秒；数字地址不启动DNS子进程，域名解析使用短生命周期进程，超时kill并wait回收，不留下后台DNS线程。9项零网络定向回归通过；同client的FRP／有限驱动51项全部通过，随后追加真实localhost解析子进程、HTTP完整正文和原ID对账单项通过，所有解析进程已wait回收，不声称一次全52项运行。

生产71个输入仅main变化：当前main SHA为`dd3952c41eb1d861d7f6d52175fa29286427a20a131038df225e518f96ebcdfc`，新输入集合SHA为`cfd25f494a48d15a125c4b1b601b8dcb7cba93d223bc749f87c944855e861f53`。另外70项、SDK与每target866项managed输入保持；旧2128／1924成员归档不改。当前修复的两目标签名和独立观察A/C四构建均已新鲜全量构建、官方app／ESP32分区验签通过，旧制品不继承为新版本。

| 当前软件目标 | signed bin／B | signed bin SHA-256 | 静态DRAM used／B |
| --- | ---: | --- | ---: |
| esp32c3 | 1052672 | `c0b6154aff5dc2f89abed46093a08d13c587079db088c873dc0dc6e6b942c50e` | 95604 |
| esp32 | 983028 | `2acb842ebb9ea6ff8918f54080951abfcaddc77e05c882f90933c525858765ad` | 42210 |

两目标每槽仍剩913408／983052 B，静态DRAM相对前版delta0；编译器实际源码942／952项和新nm均无退役运行符号／源码。有效sdkconfig逐项与前版相同。新bootloader保持21472／23008 B但完整摘要不同，按新制品分别绑定；锁定SDK包含构建日期／时间，不把完整差异武断归为单一原因或沿用旧boot身份。新SDK check与866 managed前后核对通过。C3首次configure因与ESP32争用官方组件缓存index.lock失败，日志保留；等provider结束后在新目录重试、串行configure才取得成功，没有手动删除缓存锁。

当前构建收据SHA为`6bc3781d086c88cbe00baa62475edbfd3598528388cd0dd5c0567889a897d45a`，新独占归档`receipts/private/native_callback_builds_20261006`包含1876成员，manifest SHA为`923013ab0f4421e7786f64a0f8fa0ff366e686de916f56f58c0ccd474143d8ad`。71源、双866依赖、新ELF／bin／map／分区／boot／编译输入／配置／命令、公钥、SDK检查及真实缓存失败均逐项摘要／私有权限回读，Git ignore隔离。

数据竞争红例、连接超期红例、当前生产回调源／二进制／两target host及TSan、9项定向和51＋1后继日志另存`receipts/private/native_io_regression_20261006`：44成员，manifest SHA为`89c1d72af10a5ba33df09de9d20774b2d171e5bb8def6ff6fa40adb1868298bd`。首次归档检查发现10个自动创建中间目录不是0700，未取得归档通过；原副本改名`native_io_regression_20261006_incomplete_1`并记录未完成，修正创建器后新独立副本全部摘要／文件0600／目录0700通过。原失败日志和旧证据均不覆写。

当前观察A/C绑定新71输入，仍为`0.2.0-capacity-lab`和原软件测试信任；四次full make与官方验签通过。每target的A/C分别重构941个源文件并核对完全相同，完整sdkconfig原字节相同；编译源943／953项。仅验证生成汇编首行对应本build内binary的路径注释差异，嵌入原字节也相同，不用宽泛替换掩盖源码分叉。

| 当前观察目标／副本 | 完整signed／B | 完整signed SHA-256 | 每槽剩余／B |
| --- | ---: | --- | ---: |
| esp32c3／A | 1118208 | `40b7b27dfbf6a94f6c7ea1764452ef96e423cd9ac45c48230d1b65cded53001f` | 847872 |
| esp32c3／C | 1118208 | `b144c2ceed29600f36112558f5154fdc114ebc7fec01f4e53a8166e225bd5f2d` | 847872 |
| esp32／A | 983028 | `8de8f5e5868d02a9c6af0bfe2fac14cef95d666ee10da21bb72ee0cf5b45a1fc` | 983052 |
| esp32／C | 983028 | `c9dcebc03b083c5f0caa6a57bbb2d65fd6ad055f8ca982eb9030442f45ebb8d0` | 983052 |

当前静态DRAM重新测得100220／46826 B，相对本轮普通固件均增加4616 B，不加回；声明工作区4253 B，实际linked4245／4253 B。trace、pre-deletion与SDK static cleanup开启，stats formatting显式off（普通配置中该项不展示），其余配置逐项一致。C3观察signed比旧观察增加65536 B：官方unsigned从1048576增至1114112 B，实测最后secure-pad前offset增加84 B跨入下一64 KiB界，padding从16增至65468 B，再加4096 B V2签名扇区。原始segment／SHA解析、官方build.make及固定esptool公式均保留；不把84 B全部武断归于main或日期，也不将填充增长算成RAM成本。

当前LAB构建收据SHA为`16a4fca3bb6879d2ff25e41db43c63fbdb35902193d1b5e83b78fe2a322dfb74`，pair收据SHA为`22370936040ec2c12926fa14033796694177fe6c3ff6758c2feac28303ee60d8`。新独占归档`receipts/private/native_callback_observers_20261006`共502成员，manifest SHA为`a7623245490426770d8e593dad1e80db3b871844aabc5377c7219af746641048`。75项本地生成源／副本与完整941项清单、原prep、四ELF／signed与unsigned／map／boot／表／配置／生成汇编和原binary／编译输入／nm／size／有限步骤命令及日志均逐项摘要／权限回读；866 managed通过当前1876成员归档精确引用且独立重构核对，不重复复制四遍。无symlink或硬链，没有私钥。

新A/C是软件准备输入，分别待真实R5；采样、正常cleanup和OTA-before-done事实仍不能覆盖全域瞬时峰值、所有异常退出及非任务栈。R5实板容量／断电、R6百次／连续72小时和Flash、R7删仓／正式交付均保持未完成。

Tool另外发现真实ESP32签名分区表关闭MD5，而有线Go／App原fixture强制MD5，导致真实输入被拒绝。目标格式已硬切为C3含MD5、ESP32仅FF尾并独立验证V1签名，原3072正文及68签名字节的跨Go／Swift回归已通过；此前App111／113只代表旧fixture快照，真实原件对旧版的一项失败日志保留。新App115项为113通过／2实板跳过，两个Go包通过，Universal本地无Python包门通过；六路径源码已单独保存为Tool `a1dde1a237964f7a0ac51ea4acde52b2d9b61ef5`，SDK540和Core e1不变。Root回读新374源码、实际包8成员和3日志相符，并独核Tool补验393文件私有归档，输入及摘要见[分区软件补验](../../../esp-tool/docs/operations/native_software_checkpoint.md#esp32-分区格式与原签名字节软件补验)。Server完整app验证不解析分区表，没有同类缺口。生产信任根、签名方案、分区几何与旧来源备份审计保持各自合同；软件测试公钥仍被生产入口拒绝，没有实体或生产资格。

## 固件集合读回与 OTA 终态补审

本节是上述短 Flash 交接修复后的新软件版本。进一步检查发现，固件集合观察中的 Base 自有分区读取、应用描述及 rollback 可用性查询仍直接调用 SDK，未取得短 Flash claim。当前这些自有物理访问均通过既有 `flash_io.acquire/release`；短 claim 未取得时不执行对应 SDK 访问，自有读取／查询或释放失败返回 `UNCERTAIN` 并清空输出，失败前已有的合法读取仍属本轮过程。eota 内部已拥有短仲裁，外层不再包整段锁，避免递归取得；没有新增锁、任务、缓冲或常驻成本。

修前源码在 C3／ESP32 的新测试中均编译通过、运行因无 claim 访问而断言失败；修后覆盖四种实际固件集合、26 个 acquire／release 失败点及四种 BUSY，两个目标的定向与完整 host ASan／UBSan 全部通过。完整 host 使用本轮精确 managed 目录；canonical 默认旧 managed 缺 `emqtt_contract.c` 的初始装配失败没有计作通过。实际 Flash 时延、RTOS 调度和断电资格仍须 R5。

公网宿主客户端另复现：旧实现会接受提交时相同 boot 的成功收据，也没有独立核对当前运行镜像。当前 `ota-start` 只有在原 ID 持久成功来自新 boot，且独立 `firmware.status` 在相同新 boot 精确匹配完整 signed 摘要、尺寸、target 和 OTA 槽时才返回成功；字段类型、非零摘要和目标槽几何严格核对。原 ID 查询与身份核对共用原终态绝对期限，超期不再打开连接或额外获得五秒。身份不符、迟到或未知仍保留原 ID，不重复提交。冻结旧 client 的两项新测试产生七个失败断言；当前完整 FRP 28／28 通过。

有限生命周期驱动还复现双真实线程正常递增时钟被调度顺序误判为进度回退。当前在既有时钟锁内原子采样、更新进度与读取／修改 OTA 窗口，Journal 和异常处理在锁外；真实回退、休眠和普通十秒空档仍中断。修前单项失败，修后 31／31 通过。容量解析旧版会忽略真实观察器启动声明，缺失／错 target／重复启动或 uptime 回退也可能把三个观察门判为通过；当前绑定完整真实启动声明、明确 target、唯一启动和非递减 uptime，跨文件保持同轮约束，原始日志只读。修前六个断言失败，修后容量 13／13、真实观察器双目标 C／ASan／UBSan 8／8 通过，`full_peak_or_native_lifecycle_qualification=false` 始终保持。

本轮四套共 80 项软件回归的完整日志、执行命令和前后 11 个消费源摘要均已核对。生产源 `esp_base_ota_firmware.c` 的 SHA 为 `e2ad7388ec41d768f9b68f1383c1517cdc4529b514b18f5cb8d7007deb92016b`，71 项生产输入集合为 `bbe264eeb14bc4989169c6a93f6e41ad39c0297b98eacfc71943e19a6ea5c67b`；相对上轮仅该生产文件变化，其余 70 项、SDK／lwIP、两目标各 866 个 managed 与有效配置保持。

### 最新普通配置软件制品

两目标独立全量 make、官方应用验签及 ESP32 分区验签通过，实际编译源 942／952 项，版本仍为 `0.2.0`，槽为 `0x1e0000` B。当前构建收据 SHA 为 `f3fab39c71d4642e19836e61154e748ed3915acf41af23cbb63c24f3fffacf5a`；旧制品不继承本轮源码资格。

| 目标 | 完整 signed／B | 完整 signed SHA-256 | 静态 DRAM used／B | 每槽剩余／B |
| --- | ---: | --- | ---: | ---: |
| esp32c3 | 1052672 | `6eef9d025f8de61f0646752df0a4ba7595382fa182809dfe466cd5ada8cba897` | 95604 | 913408 |
| esp32 | 983028 | `4c32cb5e8ce391e07017740582c54ccb7b46679480a739bdf1d473d4927801f3` | 42210 | 983052 |

静态 DRAM 相对修前增量为零。普通构建归档为 `receipts/private/native_readback_builds_20261006`，3064 成员，manifest SHA 为 `38d8546b6d93d3fc274034610885fe652d01a0895599f04089ecca76bad38ac4`；保存 71 源、每目标精确 managed、实际编译 SDK 源与生成汇编、原 binary、公钥、ELF／bin／map／配置／命令与完整日志。公开第三方 example／test 键按原样源码保存，本轮固件签名私钥与生产凭据未归档。

新 119 成员回归归档为 `receipts/private/native_terminal_regression_20261006`，manifest SHA 为 `c290a39d32bdab9e40990e592d36ffa8f54cec5efa8bc876ee3ee556e763e875`，receipt SHA 为 `cff0daa64122d0379434942783758a31c75a1fc9fd26694897ca8736d4ca342b`；源码、原红例、四套绿色、双目标 host 与命令逐项回读，文件 0600／目录 0700，无链接，旧归档未覆盖。官方 FRPS 新制品联调及本轮 LAB A/C 另记各自输入与结果，不把旧六场景或旧观察镜像当成本轮通过。

### 本轮容量观察 A/C 软件准备

四份 LAB 均以本轮 71 项生产输入和原观察生成器独立全量构建，官方 C3 应用 RSA v2、ESP32 应用及分区 ECDSA v1 验签通过。每目标 A/C 的 941 项源内容及有效 sdkconfig 原字节相同，实际编译源分别 943／953 项；两份完整 signed 摘要由独立正常构建取得，不手改镜像。同源只是后续测试输入准备，每一份仍须实板 R5，不能读取一个摘要就赋予资格。

| 目标／副本 | 完整 signed／B | 完整 signed SHA-256 | 每槽剩余／B |
| --- | ---: | --- | ---: |
| esp32c3／A | 1118208 | `85dcc16c8b52fcc086c1d51ac840f53458cbccc785d6afaa81d8213c17609fad` | 847872 |
| esp32c3／C | 1118208 | `dbdaad114b32eb33d80d8410c40a83eeeedf54b14e41f16632d4f2991cc5f2ca` | 847872 |
| esp32／A | 983028 | `d3e0f9bec59a0f82ef132cfc2a8e1dfdc8fa49943d6c6d28003f6c0edf873c6f` | 983052 |
| esp32／C | 983028 | `0a3e592c87880423c03df8c8320747ce0ef239a0a6c26e99436e033c9774025a` | 983052 |

静态 DRAM 为 100220／46826 B，分别较本轮普通固件增加 4616 B；声明观察工作区 4253 B，实际 linked 为 C3 4245 B／ESP32 4253 B。trace／task pre-deletion 与 SDK cleanup 接线保持，启动声明精确区分 target；正式 TLS、Wi-Fi、MQTT、FRP 双流与最大记录等预算不降低。五秒采样、已观察正常 cleanup 及 OTA-before-done 不能证明全域瞬时峰值、异常退出、非任务栈或所有内存能力域，任何观察成本都不加回。

LAB 构建收据 SHA 为 `2c55b0c1376c712fed6b43274c90adbe24af736e428111ae04d055eb2ba4305f`，pair 收据 SHA 为 `951cedfe3f59c80b7795435c94371297addceb753f3d81e1415111faf6054bc3`。新独占归档 `receipts/private/native_readback_observers_20261006` 共 4990 成员，manifest SHA 为 `f0ae1ed67f85213001a1bab0098d8fd4434fce95adf72b212f759abb8f321e6d`；四份原源、managed、实际 SDK／生成编译输入、ELF／signed／map／配置／公钥／命令和完整日志均逐项读回；unsigned 仅保留在原构建目录，不列为这份归档成员，文件 0600／目录 0700、无链接，本轮固件签名私钥与生产凭据未保存。公开第三方源码 fixture 保持原字节，旧归档均不改。

最新普通配置、LAB A/C、80 项工具回归与官方 FRPS 新制品六场景的软件证据已分开冻结并全部完成独立复核。Tool／Rust SDK 的硬切与验收仍由独立合同跟踪，本轮 Base 软件结果不能替代其资格。R3 真实公网与本机 USB、R4 Rust 主机装配、R5 双板完整容量／断电、R6 每板百次／连续 72 小时／实际 Flash 寿命，以及后置 R7 删仓和正式交付均未完成；本轮没有连接、刷写或发布任何设备。

上述 80 项归档形成时，消费源与对应收据逐字匹配；当轮源码／文档定向差异格式检查通过。工作区链接检查初次因 Tool 正在硬切的六个旧远程引用失败，后继修正后覆盖 1034 份文档／4504 个引用，缺失为零；初次失败不计作通过，最终原始摘要保存在本轮本机日志。后继驱动变化与新结果见下节，旧 119 成员归档保留原版本。

## 有限驱动收尾与观察终点补审

最终 ready／firmware 与 R5、容量原件摘要及已绑定前缀核对后，原驱动直接取结束时刻，没有再次核对宿主进展。真实 `run()` 的两模式故障注入复现：最终核对增加 20 秒空档或墙钟跳变仍可能输出 completed；cycles 模式的单调钟回退甚至输出 completed／负时长。soak 回退虽因总时长不足而 interrupted，却没有说明时钟异常。全部结果始终为 `qualified=false`／`r6_passed=false`，故没有错误赋予实板资格，但有限宿主观察的完成状态仍不正确。

第一步修复在原件核对后调用既有 `observe_host`，随后补审又复现检查后另读 `clock()` 的末端窗口：摘要可以纳入检查之后未观察的空档或回退。最终实现让 `observe_host` 返回锁内实际核验的单调钟采样；正常 `ended` 直接消费该返回值，异常在同锁读取最近已记录的原始采样，不再另取未经核验的结束时间。`observation_end_scope=last_recorded_host_clock_sample` 同时准确覆盖正常终点和采样前失败；真实回退的负时长保留，已有主异常优先，收尾日志失败不覆盖它。采样结束后的摘要／Journal写入与时钟变化不计入观察时段，也不宣称被检测。沿用现有锁与检查入口，没有新增任务、计时器或检测循环。

当前驱动 SHA 为 `26451cf512efa6d1ba76669c530c3e651f1c8085ea6ff57d211c110a0b0ddd41`，测试 SHA 为 `8bcf8e3c060df66a0916798ce27f0a6dd3b67960cffda0de7079e6434010b8f6`。完整 36 项回归通过；两模式最终核对前的六种异常全部 interrupted，核对后的六种变化均保持摘要终点等于已检查采样；单调钟／墙钟采样前的 `OSError`／`KeyboardInterrupt` 八个子例准确保留上次记录采样及原中断。第一版探针缺进展、末端 scope 红例及一次 36 项运行中四个夹具子例缺 `Clock.advance` 的失败均保留原日志／退出码，没有记作生产回归通过。最终测试只修正该注入夹具的真实时钟能力，驱动行为未再变化。

最终收据 SHA 为 `728705653bda6a25bd3abbb0c93308b72aac0e5bd766a856f37d5d659aa2f65a`，36 项绿色原日志 SHA 为 `a91d13ca290005d38dac46b6e9913f615f5718004e9cc377c70fa93824e73939`。六项驱动／FRP／容量源码与测试前后摘要稳定；FRP 与容量解析源保持前节版本，其 28／13 项通过属于此前相应运行，不把它们与本轮 36 项混称一次新全套执行。该变化不影响 MCU 或 LAB 构建输入，没有重新编译、签名、连接或写入设备。

新独占归档 `receipts/private/native_final_clock_regression_20261006` 的 manifest 登记 68 成员，不含 manifest 自身；manifest SHA 为 `3039da5d5861a595b6f099e327eed51e4e2b32abc24f9279f5342409e2e7f528`，archive receipt SHA 为 `97c39a82bc9a376227eb7fa5fa088eff50f07a589a22f21f66edf7873495a09c`。60 份完整历史及最终原件、六项当前消费源、采集器与归档收据逐项回读，文件 0600／目录 0700、单链接、无符号链接并受 Git 忽略；旧 119 成员回归档未改。最终源码、原 36 项完整日志、全部归档成员及六项 canonical 输入均已独立只读复核。

驱动只绑定外部容量原件的设备号／inode、完整摘要及起始前缀，保持 `capacity_verified=false`；它没有调用单启动轮容量解析器，也不把多次 OTA 启动拼成同一容量观察轮。容量解析器继续要求唯一启动和 target／uptime 一致，实际每轮容量与原件由 R5/R6 裁决，不新增分段器或改变资源门。R3–R7 尚未完成的实体、主机装配与交付条件保持。

本节源码／文档定向差异检查通过；最新工作区链接检查覆盖 1034 份文档／4521 个引用，缺失为零。Tool／Rust SDK 继续按已授权分工独立实施；本轮结果未给该调用链、双板实体或正式交付补发资格。

## 一次性迁入输入拒绝补审

实板就绪补核发现：`prepare_native_layout.read_file` 与其实际双备份消费者 `preflight_v3_migration.open_backup` 先阻塞打开文件，再执行普通文件判定；现役 ESP32 AT 归档的 `read_backup` 同样如此。误传无写入方的 FIFO 时无法到达输入拒绝。修前六种真实读取角色均已进入读取调用，却在两秒观察窗口后仍阻塞；测试只向各自创建的子进程发送 SIGTERM 并完成回收，原退出码和日志保留。新增真实 CLI 回归的修前超时同样保留，没有计作通过。

当前仅在上述三个输入打开点加入 `O_NONBLOCK`，沿用原 `fstat` 普通文件、权限、尺寸、摘要与关闭规则。原生准备仍消费既有双备份比较链，旧AT归档仍保存原字节；不改变签名、目标、身份、旧未决阻断、配置保留、输出防覆盖与读回合同。AT输出归档的 `read_archive` 保持原版本；生命周期驱动已有非阻塞读取，本轮未改。没有新增读入机制或运行面。

| 当前输入源码 | SHA-256 |
| --- | --- |
| `prepare_native_layout.py` | `4a682771b1a837b19ac865bd612d8348cb20a79ce00859b81fec1e8a4ffe56ef` |
| `preflight_v3_migration.py` | `7280c3ceedb31c2c21dea820f87382000f80b6045bbbe63c32fc5509db6302f6` |
| `archive_esp32_at.py` | `deed3821e9f7c853d4ce845425e7f5386a96f1f53ef348294c43a9816a08eb33` |

原生布局、旧C3预检和AT归档完整测试分别9／13／13项通过，共35项；11个真实CLI输入位置均在五秒测试上界内明确拒绝且未生成候选／归档输出。同样六种直接读取探针约0.07–0.08秒返回拒绝，已打开的fd全部关闭，未发送终止信号。测试覆盖原生七个输入位置及旧预检／AT备份各A/B；三个生产helper相对修前快照逐字核对为各一处打开标志变化。MCU、LAB、FRP与生命周期驱动输入未改；本轮未重建或重签此前冻结的MCU／LAB软件制品，未操作设备。

最终收据 SHA 为 `c304e58bde1661d08dbb05fb54b2aa452f44142468f4a9d31f3b711a3c10eb0f`，私有原件共74个普通文件：58份控制／失败／最终材料、8份公开合成signed app、7份RSA3072公钥、1份全FF比较输入。八个遗留scratch文件的0644→0600权限修正单列记录，原字节保持；FIFO只保留真实元数据、调用／回收日志和清理收据，未当作普通文件成员。新生成的测试私钥由既有suite清理，未复制到原件树；未读取生产凭据，合成签名输入不作为正式候选或实板资格。

新独占归档 `receipts/private/native_fifo_regression_20261006` 的manifest登记82成员，不含manifest自身：74份原件、六项当前源码、采集器和归档收据均逐项回读。manifest SHA为`111adf11a10cd08b9a00b2cc75f123ff27e78fdea0dea01dea0c0240b1ae2c2e`，archive receipt SHA为`a6f252b95ca1d966a5058318f2ac057c0e0924e99683d3e685a1093d6c357d79`，原74文件集合为`44aa8fca621307929a4b125f29c6f15bb9bc329cc3d4a7a61b9fe4ffab16d741`。全部文件0600／目录0700、单链接、无特殊节点并受Git忽略；最终源码、35项完整日志与全部归档成员已独立只读复核。原71项MCU生产输入、受保护的驱动／FRP／容量六源仍与各自已冻结版本匹配，旧119／68及其他已冻结档未改。首次私有目录冻结的权限断言原因保存在权限修正收据原字段，没有单独完整rawlog，不将其宣称为独立日志原件。

在该FIFO补审阶段回读时，SDK canonical HEAD及Tool索引`macos/dependencies/esptool-sdk`的gitlink仍为`5406589a54ca0e0ef51cc462c15ed773aee2b698`；彼时配套聊天为active／inProgress，正在全量回归与最终SDK／App冻结装配。这是当时的输入状态，不代表后续Rust精确消费的当前版本。旧SDK29／Swift阶段证据不赋予Rust资格。实板身份／4MiB／安全状态、正式信任、新鲜恢复基线、唯一租约及R5／R6真实原件仍需分别取得；R7删除保持后置。

本节源码／文档定向差异和新消费源摘要核对通过；最新工作区链接检查覆盖1034份文档／4524个引用，缺失为零。上述软件输入拒绝补审未扩大R3／R5／R6实体或正式交付资格。

## Mac 远程功能删除软件边界补核

本轮独立只读核对配套Tool已冻结的`provisioning/receipts/private/local_device_cutover_20261006/swift_stage`。软件收据SHA为`7a20ca06187cdbe29d5e7ed94ff9826e642baa53007b9d1c8c1f3c5faad836f8`：369 payload包含352份完整工作树源码、11份软件日志及6份App成员，另有收据自身；全部成员SHA／尺寸／0700-0600／单链接与原件对应通过。`source_head=a1dde1a237964f7a0ac51ea4acde52b2d9b61ef5`只是该阶段HEAD，删除源码属于收据绑定的工作树快照，不宣称已进入这个提交。

冻结Mac源码、UI、配置／配对授权、工程与随包消费已移除HTTPBridgeServer、FRPClientController、helper／frpc、BridgeBearer和远程bridge路线；本机设备动作直接进入本地monitor，设置／导航只保留本机职责。Universal可执行文件含arm64／x86_64，两架构entitlements均仅有app-sandbox、serial、usb、user-selected.read-only与network.client，没有network.server。六成员共4248315 B，包内无frpc／helper／Python；可执行文件SHA为`a3bab5037e4692aeb9c2de381cfae867aa8eceb744e3214671ba2fdd8dba4fb9`，bundle冻结JSON为`0e6937e55e4fb85dfffdec2684b748e0e4757798d5bf2e561e056b353aed6a54`，gate JSON为`7ecc6bcbf515fd86dc589ea88bfc76827fff0fffed747d7a9abb91c4d563f491`，均与实际包匹配。

App原始日志97项中95通过、2实体跳过、0失败，5项本机OTA及旧Bridge proof拒绝已包含在内，不另行累加。冻结Server日志384项通过，覆盖旧远程USB／delivery路线退出、旧未决操作只读原账本而不联网、拒绝旧Bridge配置字段；这些是软件结果，不赋予生产v12迁移或线上消费者完成。包保持ESP Tool／`com.xdarren.esptool`／1.0.0／1，`ClientLocalValidation=true`、Sign to Run Locally，属于旧Swift SDK `5406589`阶段的本地验证，不能用于正式分发。

据此R3的软件删除子项可收口；实际沙盒启动、运行时无listener、真实USB／旧安装数据／租约／OTA／Flash恢复及新的Rust消费链按后继证据分别核验。该Swift阶段复核没有启动App、打开串口或执行设备操作；后续SDK全量及实体资格仍按独立合同取得。

后继Tool／Rust SDK已保存精确提交，新阶段原件、实际链接与冷装配独立复核见下一节。旧369成员Swift归档保持原样，不能替代新Rust原件。

宿主测试入口另确认默认`firmware/managed_components`是已清理的生成物，manifest和双target锁仍对齐本轮组件；官方重新解析或显式冻结目录均是已支持的复现路径。测试README已补准备条件及双target完整目录示例；本轮只改说明，没有新增组件版本机制或重跑已绿MCU／host。

## Rust SDK 精确消费与冷装配软件补核

本节独立只读复核配套任务的新Rust软件快照，收口R3现有App所需Rust迁入与R4主机精确装配的软件子项；未取得实际App启动、USB、旧安装数据或实体恢复资格。固定消费关系如下，canonical后继WIP不属于该候选，也不要求App跟随它移动。

| 冻结对象 | 完整提交 |
| --- | --- |
| ESP Tool产品 | `b748749bc5cde3db7793d6361917d056d223e18a` |
| 产品SDK gitlink／clean detached消费者 | `8db20c5a9e8bf6deddc1ee54b392bd0796ad7bc4` |
| 产品Core gitlink | `b020a35cb08a63aba989f953f2bcf9d48dd849fa` |
| 冷装配工作区输入 | `196e7f8381d9264d6ce18b29fd3fe82f27d4823a` |

Tool正式树与索引的SDK gitlink、产品内消费者和已保存SDK提交一致；69份SDK源逐字匹配Git blob，63份Mac消费源码同时匹配产品提交和实际App源码。双架构实际Rust编译输入均来自产品内消费者，没有相邻canonical import。被替代的Swift协议／寄存器／串口／MD5引擎已删除，Swift只做候选发现、C ABI调用与类型转换，不保留Python或外部esptool兜底。

原始SDK日志84项Rust、11项Swift薄绑定及5组C++通过，5组C++ ASan／UBSan通过；Rust覆盖现有C3／ESP32 ROM／4MiB Flash软件子集的目标／安全拒绝、累计预算、取消、ACK未知、MD5、回调寿命与写后恢复事实。App最终97项为95通过、2实体条件跳过、0失败；24项Executor只消费高层会话fixture，ROM／ACK／MD5由单一Rust回归验证，不把它们当作实体Flash结果。

实际Universal App恰6成员、7676219 B，可执行文件SHA为`0d26b6f7ea15c3b25fa9685a71e45ae3bbb1e5203b9e6ad219f131640bd3d55d`。两Mach-O架构各有同组19个SDK定义导出，直接解析ABI函数均返回2；Xcode两架构实际链接产品内`libesptool_sdk.a`，该输入为36716376 B、SHA`75ce71409f06e0c0e1168bb4008801eccb7f45100f38210bfd4c7306cc6d7fad`，与消费者同源Universal库相同。两架构签名内权限均恰为app-sandbox、serial、usb、user-selected.read-only及network.client；无network.server、frpc、helper或Python。保持ESP Tool／`com.xdarren.esptool`／1.0.0／1／`ClientLocalValidation=true`，仅为本地验证包。

冷装配从空SDK源码目录获取完整提交并重新构建Universal核心，Root SDK保持未初始化；冻结App与薄绑定通过SwiftPM编译。1030份tracked输入分别为产品351、SDK69、Core221和工作区389，Git blob、实际字节／mode／mtime均独立核对。重复准备前后这些输入不变；向SDK README追加真实脏marker后，入口拒绝并保留marker，夹具finally恢复原字节和mtime，最后再次匹配原快照。初次临时目录别名路径拒绝原日志也保留。此冷构建使用隔离的既有Cargo缓存；最终Universal Xcode App是同提交的另一份装配证据，不称完全空缓存、完整离线重建或冷目录中的Universal App打包。

新独占档为Tool `provisioning/receipts/private/local_device_cutover_20261006/rust_stage`，`software_receipt.json`为199970 B、SHA`69880f7caac8694f903a63533ae1dda711bb6307b45016ebb925dcd40e73b5a3`；核前核后摘要稳定。452 payload共20015305 B，包含351份产品Git blob、69份SDK Git blob、4份装配事实、6份App与22份选定日志，另有收据自身，实际453文件／147目录。全部成员逐项摘要／尺寸／原件对应、独立inode、单链接及Git ignore通过，文件0600／目录0700，无链接或特殊节点。冷装配收据为244645 B、SHA`72f706fbd8ad75723ab50f533bbf93b862208401f80fd0a1e7963e183a1b2cc6`，与归档中的共同源集合和实际冷目录相符。本档明确仅保存精确源码和选定装配事实，不含完整构建缓存；上述Universal SDK库是实际链接核验输入，不列为该452成员档中的独立库副本。

初次App Rust切换编译红日志`/tmp/esp_tool_local_mac_rust_swift_test_20261006.log`为18611 B、SHA`3af878eb4fedea1628edb5de1ae7129a65642c0ed7dd5d52442de66dea03b549`，原因是旧ROMProtocol／CommandConnection测试引用；迁到既有高层产品会话接口后才取得最终95通过／2实体跳过。原红不记为通过，也不在上述22份日志中。配套任务已单独保存于`rust_stage_supplement/initial_rust_app_compile_failure.log`；补件收据727 B、SHA`6634f461ff52abb35a9dd2ed6d5ee1cd8cca3e0dccfdff9ca53a69e5b0aa26f3`，明确绑定原452成员收据。Root只读核对补件字节／尺寸、0600／0700、单链接、独立inode与ignore通过，原临时日志仍在；稳定452成员档摘要未变。

据此R3 Rust软件迁入及R4精确主机装配可收口。完整SDK四工具／全部适用芯片／三宿主／stub／Qt／安全制品资格由其独立合同继续验收；本机实际启动、旧安装、USB／取消／Flash／恢复与公网实板按R3／R5取得，R5容量／断电、R6百次／连续72小时／实际Flash寿命、R7后置删仓与正式交付仍未完成。本节未重新构建、重测、启动App、读取生产秘密或触达任何设备。

最终定向差异、空白及新增引用检查通过；工作区链接检查覆盖1038份文档／4544个引用，缺失为零。任务总览初次差异检查因并行保存产生的三处冲突标记失败；后继仅保留冲突块内四条较新进度并合入本项结果，块外原字节不变，清除标记后工作树与索引差异检查通过。已标记该文件冲突解决，没有继续其他聊天的Git合并／提交／推送流程。

后继R3实际启动可执行性补核：冻结App入口无条件创建BridgeStore；其初始化创建真实安装容器的wired-flash／firmware-ota目录、读取旧记录并启动监控。监控自动枚举候选，取得租约后调用Rust ROM探测、切换DTR／RTS并打开串口；未决OTA也可能进入本机串口。生产App没有关闭扫描或隔离安装数据的启动参数，测试内部allowedPorts和硬件跳过变量不属于App运行入口；AppConfig从getpwuid读取真实用户home，不能用HOME环境覆盖冒充隔离。核对来源为Tool `b748749`的BridgeStore／SerialDeviceMonitor／AppConfig及SDK `8db20c5a`的ROM调用链，当前源码仍与该冻结候选相符。因此实际启动须与当轮设备、唯一租约及旧安装数据窗口一并核对，继续等待维护者已安排的实板后置窗口，不把包／host回归称为实际沙盒运行。此补核未启动App、枚举USB、打开串口、读取Keychain或修改旧安装数据；未新增运行开关。归档中的0600可执行成员是证据副本，实际启动须使用已核原始本地App包或同精确提交重新构建的受控包。

后继有限阻塞审计确认：当前R1／R2／R4软件项和R3既定软件子项已有各自冻结证据；没有发现本轮约定内可独立推进的必要软件缺项。尚缺同一实板／安装窗口，连续三轮未解除；R3真实运行、R5／R6及后置R7仍需外部条件，Goal转为blocked，完整目标不变。没有用独立SDK全矩阵、LAB发布门或重复回归扩展Base前置，也未启动新设备任务、修改有效凭据或提前删仓／发布。
