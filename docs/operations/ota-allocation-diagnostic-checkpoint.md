# C3 OTA 分配诊断检查点

## 2026-10-05 MQTT 消息所有权诊断前置失败

为归因此前最大事件／OTA 的堆缺口，仓外诊断取公开 Base `b677868cc6a772351c22019818f02cbf8bcda020`、原 Container 与 MQTT `6443b71db761f4d667503f14108687bad5e6b5ee`。MQTT 实际编译输入显式指向独立实验组件，只添加 48 B 原子消息 owner／错误计数；释放开始前撤销计数，不能用它证明 native free 完成。队列、载荷、分配、优先级、协议与期限保持。另通过私有 CMake 令 SDK 既有两类动态 TLS 日志可见，SDK 源码未改。最终实际 ELF 相对前轮：data +48 B、bss +8 B、Flash text +8,832 B、rodata +2,056 B、普通堆起点 +48 B，观察成本不加回；此前 owner-only 成本记录不能替代此最终日志配置。

两份实验 A/C 均为 1,183,744 B，版本 `0.2.0-c3-mqtt-owner-a/c`，完整签名摘要为 `33bde2d61432f954fd317c1d031f255adbf38b146fa9671c0385999711a2a325`／`903fe67ff830d185ebb1f1293259d9c240939b1b41f0fbbb3a11d0f8f436bfd7`；完整 SDK 构建、官方实验键验签与 ASan／UBSan host 计数平衡检查通过，不取得生产实板资格。

**两轮 driver exit 1，均在首个 MQTT status 查询的五秒宿主期限处失败，尚未执行突发或 OTA。** 后到回执逐项证明同请求、同设备、同 boot 的 succeeded，不能再描述为设备未返回结果。第一轮只在后继 OTA 阶段准备持续 reader；第二轮改为从首个 USB 查询开始单 reader，OTA 前顺序交接、重启后继续采集，设备调用 AST、原失败捕获与恢复体保持。第二轮 logged／consumed 均 131,676 B、pending 0、峰值 25,167 B、overflow／reader error／join timeout 均无；仍复现超时，停止读取不能单独解释该现象。详细日志的时序影响尚待隔离，不能据此认定前轮峰值中的 MQTT 重连原因。

每轮均重新核对唯一 C3／4 MiB／安全状态，以本轮两份一致全片和独读原三码建立恢复基线；实验镜像全片读回通过。两轮结束均擦实验数据、精确恢复原 bootloader／partition／factory、确认 Wi-Fi down、所属夹具关闭与串口释放，cleanup errors 为空，没有 eFuse 或生产配置变更。80／51 条同 boot 消息 owner 周期行均为零峰值且申请点仍为未观察哨兵，不能作峰值占用或回收结论。

失败材料在 Tool 既有 Git 忽略目录中独立私存，目录 0700／文件 0600，逐成员 SHA／长度／权限核对通过：第一轮 180 payload，归档 `820e1e7a65a266994428cd73c16a852cc3b39bc312d8e24556cc7b02e35aa9cc`；最终源码补件 14 payload，归档 `9b1731f40de8516d2935e53e342f7b99180dbeae5136e6996317be6d3ad154e0`；第二轮 189 payload，归档 `75c81378ed03335d2e9b08c6f19fd332639833ffacede922867cf78d0d4cb090`。原最大事件／OTA 的容量失败保持；下一轮仅保留消息计数、关闭详细 TLS 日志，并继续使用新目录、新鲜双基线和原恢复合同。

### 关闭详细 TLS 日志后的消息 owner 实板续验

独立静默候选移除私有 SDK 日志编译参数和两个运行日志级别设置，仅保留消息计数与原周期资源观察。公开源码逐文件核对，固件差异严格限于已声明的观察、实验策略和实际本地组件装配；首轮发现三个旧文档输入后按声明的公开提交重新取回，固件构建输入保持。实际 MQTT 编译目录、C3 的 A/C 配置标签、SDK 源码和官方实验验签通过。A/C 均 1,183,744 B，版本 `0.2.0-c3-mqtt-quiet-a/c`，摘要 `0ac9d7acf9699637fcd65f49adcceedd2b6bc4a6e3c3501de60f69110c54868f`／`da593513360a378bb4667ee8119983e0bc6266eb522b6225c2cca2b9cd9a7b89`。相对前轮 queue-only ELF，data +48 B、Flash text +628 B、rodata +312 B、普通堆起点 +48 B，其余分配段保持；成本不加回。

**本轮越过前两轮的 MQTT 失败点，但整轮 driver exit 1。** 新鲜双全片、原三码独读和完整实验镜像读回通过；公开 USB Wi-Fi／MQTT 首配、严格 TLS 就绪、MQTT status／product.status、坏 HMAC／QoS0 拒绝后的合法同 ID 查询，以及远程 config.set 的 physical_usb_required 拒绝通过。随后 revision 3 的 USB 状态已报告 FRP／MQTT ready，首个外侧 FRP status 探针仍不可验证：私有 HTTPS 转发夹具记录其本地 FRPS 上游 TimeoutError，未得到设备响应。具体停在哪个网络阶段及其原因尚未取得证据；保持原三秒响应头、八秒完整查询与设备期限，下一步补齐宿主上游阶段观察。

62 条严格消息 owner 观察中峰值为 1 个／2,154 B，所列 allocation／满槽／notice／poll 错误计数为 0；产品队列准入仍为 0，尚未安装产品、触发突发或执行 OTA。62 个完整任务快照及 62 个周期内存样本无损坏／缺失；这些低负载读数不替代既有最大事件／OTA 的容量失败或完整 native 资格。详细日志的时序影响仍需独立核对，原容量轮的 MQTT 重连原因继续未证明。

UART 138,341 B 全部记录和消费，pending 0、峰值 20,622 B、overflow／reader error／join timeout 均无；单 reader 已退出。原三码由本轮新鲜全片独立复核与恢复字节完全相同；实验数据擦除、Wi-Fi down、串口及夹具释放通过，cleanup errors 为空。静默软件 43 payload 归档 SHA `480bc8029ebb9563b02ac87ad33e6d3bdd21f52cc77903bc465dcd974ff2f1b7`，完整现场 137 payload 归档 SHA `6976ad5f86f55f11072156bf3a88e552b4c2c82ff064c6ba031f5679a85fb9b1`，各自逐成员 SHA／长度／权限核对通过，继续受限私存于 Tool 的既有忽略目录。生产材料、完整容量及迁移资格保持原边界。

## 2026-10-05 当前容量合同与续验范围

维护者在本轮明确确认：ESP32-C3 与 ESP32-D0WD-V3 均保持 Wi-Fi、MQTT、FRP、OTA 和 Wasm 五能力并存，普通内部 8BIT 堆的历史最低门槛统一改为 **16,384 B（16 KiB）**。普通堆最大连续块至少 **24,576 B**、各任务栈余量至少 **1,024 B**，并按实际能力域的后续最大申请验证连续空间；严格 TLS／签名、标准 Wasm、队列、包槽、期限及既有功能预算保持。

本轮修改验收合同，不产生内存节省，也不追认历史轮次。以下历史记录继续按各轮当时的 48 KiB 合同保持原失败结论。最新 C3 新鲜联合功能轮绑定 Base `f2f9ad24aadaa46ec358d7fef23a8950ba4a3092`／Container `7f25647a5380953dfd40efcef281014660ff9530`，覆盖此前签名期限、制品身份、OTA 恢复、必需能力、定时业务与映射只读修正；完整合法峰值仍须补验。当前签名软件镜像为 C3 1,183,744 B／ESP32 1,114,100 B，各槽余 61,440 B／65,548 B；源码、实际 ELF／bin 摘要和固定 SDK 已只读核对，不能代替新源码实板峰值。

- [x] ✅ 维护者确认双目标统一 16 KiB 堆门，五能力并存及其余合同保持；现行计划按 target 同步。
- [x] ✅ 当前冻结组合的新鲜 C3 联合功能轮与恢复、周期堆／连续块／已观察栈门通过，范围和私存见下节；不代表完整容量。
- [ ] 完整合法峰值／所有任务栈／连续块、native 回收、联合 Flash 争用与寿命、ESP32 实板、真实断电、双板 72 小时和正式交付。

## 2026-10-05 最大事件突发与 WRITE OTA 重叠实测

Tool 交付前置续接 C3 容量实验，绑定公开 Base `41c0eeec5ad828ab311594d639e831858bd3ddc0`／Container `7f25647a5380953dfd40efcef281014660ff9530` 和原固定 SDK。仓外 A/C 为 `0.2.0-c3-queue-peak-a/c`，完整签名 app 各 1,183,744 B，摘要分别为 `bafee24c4838b53f215cf6d80e6ca3b401349bdd23b56e01afdf720e298a7832`／`845dad1bdcfba2c403f8dcc23c20c347798ea9cb8125e553b7dda7d3de17f12d`；官方重新验签通过，仅使用既有实验键／CA。SDK 和 provider 保持原字节，产品队列观察差异可逆回公开源码；24 B 固定计数使实际堆起点后移 32 B，成本不加回，未修改队列、准入、分配、优先级或期限。

**功能与原代码恢复通过，容量失败。** 每轮新目录且全程持有与 Bridge 相同的串口租约；重新核对唯一 C3、安全状态和 4 MiB，两份新鲜全片一致，独读三码与全片相同后才完整写入／读回实验 A。公开产品安装、来源单个最大事件、唯一一次 WRITE OTA 到 C 持久确认、USB／MQTT 各一次停止启动、重启自动运行 confirmed、原 stop ID 新 boot unknown、十二项业务、卸载及 A/C 精确读回完成，真正 driver exit 0。结束擦除全部实验数据，恢复本轮独读原 bootloader／partition／factory；Root 独立核对原三码、恢复字节与新鲜全片相同，Wi-Fi down、串口释放、所属夹具关闭通过，清理错误为空。没有 eFuse 或生产网络／信任材料修改。

在来源同 boot 已观察到 OTA 正文 5,056 B 后，一次发送 32 个精确 4,096 B 公开签名事件帧（载荷 3,893 B，序号 3–34），每个 application publish 只调用一次，无重发；32 个 PUBACK 仅证明控制器至 Broker 的传输确认。447 条严格队列观察中来源为 134 条，来源 queued／live 高水均为 **4**，总 accepted offers 为 **6**（含此前两事件）；产品层 allocation failure／FULL 计数均 0，不扩大成全部 native 无失败。没有达到 8 个排队事件加 1 个处理中事件，不把 32 个 PUBACK 当作 32 个设备入队或业务完成。

| 观察范围 | 实际最低 B | 当前门 B | 结论 |
| --- | ---: | ---: | --- |
| 普通内部 8BIT 历史堆 | 3,396 | 16,384 | 失败，差 12,988 |
| 周期采样最大连续块 | 21,504 | 24,576 | 失败，差 3,072 |
| 事件准入后的最大连续块 | 3,840 | 24,576 | 失败，差 20,736 |
| 控制栈／已观察任务栈余量 | 2,392／1,196 | 1,024 | 所列周期观察通过 |

四份原 UART 严格重解析得到 458 个内存样本、458 个完整任务快照，不完整／损坏均 0。来源下载 153 份状态中 Wi-Fi／FRP 全部 ready，MQTT 110 份 ready、43 份 connecting；不能称五能力始终 ready，具体重连原因仍待归因。突发前的同 boot status 历史堆／current 为 32,176／37,556 B，随后历史低水为 3,396 B；这是本轮顺序观察，不当作某一模块的独立节省。UART 755,497 B 全部消费，最高 pending 67,673／1,048,576 B，overflow 0、reader error 为空、join 无超时，没有 backend 重开。

完整现场与软件材料已受限保存于 ESP Tool 既有忽略目录 `provisioning/receipts/private/c3-validation-20261005/queue_peak_with_ota/`，248 个 payload 成员逐项 SHA／长度／模式核对通过；归档 SHA-256 `f81019d51974501082f83c750ff7f25fa1585916825f28cea3231a486b4d8616`，索引 `780484bce322072345b0af76037975ee32fac209a89029108cdfae40e7b1b162`。材料含实验网络、设备与恢复字节，仅私有 0700／0600 保存，不发布。

当前已经证明合法最大事件与 OTA 重叠可使 C3 低于新 16 KiB／24 KiB 门，`whole_capacity_passed=false` 保持。下一步核对消息、产品事件和 TLS 在同一窗口的实际所有权与占用；不进入永久迁移或正式 Tool 实板交付。满八队列及 processing、完整 native／分域申请、FRP 最大记录／双流重叠、Flash 争用／寿命、百次实体生命周期、ESP32、真实断电和双板 72 小时仍待完成。此前有限联合轮通过的证据范围保持，不重新改判历史。

## 2026-10-05 新组合联合续验与映射只读修正

双目标按本页当前合同统一为 16,384 B，周期 UART 消费者的 8 项边界测试通过，默认终端块摘要与 `--json` 机器模式分别核对。旧成功轮 474 个完整任务快照可被严格重解析；该工具重放不改写原 48 KiB 合同下的旧失败，不作为 ESP32 实板证据。

本轮以公开 Base `c8cbae4b991f73367a7e2cae80e977bea2eb6d46` 的运行源码和 Container `15b74a2` 准备新 A/C，SDK 与 provider 源码保持，仓外只含既有实验 CA／策略、周期资源和任务观察。首轮因装配时漏带 MQTT 的 `existing_network.json` 输入，在 Wi-Fi 连通后失败；两份新鲜 4 MiB、实验写入／读回及原三码精确恢复、擦实验数据、Wi-Fi down、UART 释放通过，原失败保留。补齐既有 MQTT／HTTPS／官方 FRPS 与网关材料后，三类真实本机 TLS 构造、ready 与关闭先行通过。

第二轮重新取得独立新鲜基线，MQTT／FRP 认证管理、公开安装与一次 WRITE OTA 下载通过；候选 C 打开产品时 `open_slots=3/open_runtime=3`，原策略回退至 A，观察器因此拒绝后续新 boot 不符。完整联合轮失败。157 个完整任务快照，周期历史堆／连续块／控制栈／已观察任务栈最低分别为 **23,400／28,672／2,392／1,196 B**；三个数值观察门通过，满合法峰值与完整 native 未取得资格。恢复／擦实验数据／Wi-Fi down／UART 释放通过，清理错误为空。

第三轮独立重读双基线，只增加原失败 claim 的日志，处理、期限与预算保持。候选 C 再次回退，日志 `active=22380/claim=22380/owner_bound=1/task=pthread` 与源码对应：映射包持有自己的 Flash 租约，引用身份门复读另一份保留包时，原 provider 再次获取同一非递归租约而失败。148 个完整任务快照，历史堆／连续块／已观察任务栈最低为 **25,480／24,576／1,196 B**，不与第二轮相减归因。原三码恢复、数据擦除、Wi-Fi down 与 UART 释放通过，原失败保持。

三轮全部 regular 现场分别以 119／160／160 成员受限私存于 ESP Tool 的 `c3-validation-20261005`，归档逐成员 SHA／size／mode 核对通过。归档 SHA 依次为 `a62606fb8adda7ae3262e43bcced7f16d9227538733528de5d5778c97917e4ce`、`920dd061f149332335a4d7449a4605eaf4c4d4360dfa66bc6b05a19ea2261079`、`23f70b5d43e1fbf0fe561c62d4219648847db97c1357973c8071969b4e46ae19`。本轮未写 eFuse 或生产服务。

修正已由 Container 源仓保存推送 **`7f25647a5380953dfd40efcef281014660ff9530`**。在既有槽锁内，仅当前映射任务的包分区只读复用其持有的租约；跨任务／写入不能借用，重复 map 和错任务／错 handle 的 unmap 拒绝，正确 unmap 单次释放。源码与回归正文见 [Container 检查点](https://github.com/esp-space/esp-container/blob/7f25647a5380953dfd40efcef281014660ff9530/docs/operations/three-slot-storage-checkpoint.md#2026-10-05-映射窗口内保留包复读的-flash-租约修正)。原 provider 行为红、普通 10/10 与 ASan／UBSan 6/6 通过。Base 两目标官方 manager 锁的其余字段保持，组件摘要均为 `97f94138bae86610842aa1534a3ac6013a44f94a35386bfdf1982e8d1aea013c`；host／真实签名生命周期四入口、固定 SDK 完整签名构建及 RSA v2／ECDSA v1 验签通过。补全 Base 测试 fake 的原生 mmap handle 类型后四入口通过，初次编译失败保留。

两目标签名 app 仍为 1,183,744／1,114,100 B；产品 context 各增加 8 B，分配段除 `.flash.text` 增加 136／128 B 外保持，实际非函数 heap 符号保持。新增节省 0，不继承旧新鲜轮的容量或实体资格。修正后的 C3 新鲜联合轮如下；满合法峰值／native、最大连续块／全部任务、Flash 争用与寿命、ESP32、真实断电、双板 72 小时及正式交付继续。


### 修正后的 C3 新鲜联合轮

公开 Base `f2f9ad24aadaa46ec358d7fef23a8950ba4a3092`／Container `7f25647a5380953dfd40efcef281014660ff9530` 的 A/C 完整固定 SDK 构建与官方 RSA v2 验签通过。版本 `0.2.0-c3-mapped-read-a/c`，签名 app 各 **1,183,744 B**，SHA-256 分别为 `ef71b4ea07d161fadd1ade857881b42f9fc4e0c88a25dd850ad34548fa2d8b3f`／`7f438f056499bf808eafd8dc0f6cb1e2296d1e7b00d1a74deaf65163dba3278a`。SDK 与 provider 无私有修改，仓外仅原实验 CA／策略、周期资源／任务快照与 worker 退出观察；原写序、期限、单 reader 和恢复体保持。全部输入、损坏／符号链接／误在准备目录执行的拒绝核对及三类实际本机 TLS 夹具预检通过。

**真正 driver exit 0，C3 联合功能与恢复通过。** 本轮重新核对唯一目标、4 MiB 与安全状态，读取两份新鲜一致全片及独立原三码后完整写入／读回 A。公开产品安装、来源最大单事件、唯一一次 WRITE 联合 OTA 到 C 持久确认、USB／MQTT 各一次停止启动、五个原生命周期 ID 各写一次并只读重查、停止后重启自动运行 confirmed 产品、旧 stop ID 新 boot 双通道 unknown、十二项业务、卸载及 A/C 精确读回均通过。候选 C 没有前两轮映射复读失败；最大事件仍在 OTA 前完成，不能写作并存峰值。

来源下载 **146/146** 状态样本 MQTT／FRP ready。四份已保存原 UART 严格重解析与整份原观察相等：**457** 个内存样本和 **457** 个完整任务快照，不完整／损坏均 0；历史堆／最大连续块／控制栈／已观察任务栈最低分别为 **23,464／27,648／2,392／1,196 B**。这三个周期观察门按当前双目标 16,384／24,576／1,024 B 合同通过；不扩大为所有瞬时峰值、全部 native 生命周期或 ESP32 实板。读数顺序取得、观察成本不加回，不与失败轮相减计收益，`whole_capacity_passed=false` 保持。

原 continuous UART **716,736 B＝已消费 714,904 B＋结尾待消费 1,832 B**，最高 pending 64,998／1,048,576 B；overflow 0、reader error 为空、reader 已退出、join 无超时。本轮没有发生 backend 重开。结束擦全部实验数据，只恢复当轮独读原 bootloader／partition／factory 并逐字节读回；Root 再核对三码与新鲜全片及恢复字节一致，Wi-Fi down／串口释放和清理错误为空。运行中绑定的 **3** 个所属进程及 **5** 个真实监听结束后逐项复核释放；没有全局零进程声明、不写 eFuse、不改生产服务。

ESP Tool 的受限、Git 忽略 `c3-validation-20261005/capacity16-mapped-read-software` 保存 **34,644** 个软件归档成员（固定 SDK **31,468** 项），单档 SHA-256 `1e20cf416c9d8c80d077d0aee736771012fd63c43f3a002dccb31e018b602b46`，索引 `2673377248e5ccc1db88a5a2043f5a2aa276b0e0bd0af5502ed526141703aa10`。`capacity16-mapped-read-physical` 保存完整 **192** 个 runtime regular 文件＋1 个索引，单档 `df255be582af9b0f506e384e2e1ee900409ec86bb9ca8574d826fceaea4819d5`，索引 `3f5d419c75bfa809c0646a6d4f5ecc27c51452633ab72601ba93020f378da2a2`。两档逐成员 SHA／size／mode／集合及源文件前后不变核对通过，前三失败独立保持。

该轮关闭本组合的候选启动与有限联合功能复验，容量完整门保持开放：合法满队列及 processing、MQTT 满槽／在途／outbox、FRP 双流／预备流／64 KiB 记录与 OTA 相遇、完整 native／所有任务／分域最大申请、联合 Flash 最坏时延与寿命 Y/f、实体百次生命周期、ESP32、真实断电、双板 72 小时及正式交付仍待完成。


### 产品队列峰值观察输入与 FRP 首查前失败

在公开 Base `41c0eeec5ad828ab311594d639e831858bd3ddc0`／同一 Container `7f25647a` 上，仓外增加产品队列高水、原 OOM／FULL 次数和入队后普通内部连续块观察；写方复用原队列锁，周期读取快照后解锁再打印，并绑定真实 boot。反向删除全部观察增量后产品源码逐字等于公开输入。原申请、准入、队列、调度优先级与期限保持；SDK／provider 无私改。实际 ELF 固定观察区 **24 B**，`.dram0.data` 增加 24 B、`.flash.text`／`.flash.rodata` 增加 248／160 B、`_heap_start` 后移 **32 B**；全部成本计入本轮读数、不加回。

A/C 版本 `0.2.0-c3-queue-peak-a/c` 完整固定 SDK 构建与官方 RSA v2 验签通过，签名 app 仍各 **1,183,744 B**；SHA-256 为 `bafee24c4838b53f215cf6d80e6ca3b401349bdd23b56e01afdf720e298a7832`／`845dad1bdcfba2c403f8dcc23c20c347798ea9cb8125e553b7dda7d3de17f12d`。与前节 A 的实际 sdkconfig 仅软件版本标签不同，数字预算及原实验 CA／策略保持。来源突发驱动只在同 boot、已确认原 counter、前两事件完成、正式 OTA 正文已经开始时触发一次 **32** 条 QoS1／非 retained 最大事件，guest／wire 长度 **3,893／4,096 B**，连续序号 3–34 各应用发布一次；PUBACK 不当入队／执行证明，超时不重发。发布器与高水解析器 **8 项**边界测试通过；首版 mock 全零 key 被公开协议正确拒绝的两个错误保留，修正仅 mock 数据。原主写序、原异常捕获与恢复体、单 reader 和 1 MiB 宿主队列核对通过；新主流程仅增加已声明的来源事件突发。

**三轮 driver 均 exit 1，均未到产品安装、事件突发或 OTA。** 每轮重新枚举、确认 ROM 身份／4 MiB／安全状态，取得两份一致 fresh 全片和独读三码，再完整写入／回读同一 A。USB 配置、严格 MQTT、FRP 配置 revision 3 与 ready 观察后，原首个 FRP 认证 status 未取得可验证结果，夹具网关报 `TimeoutError`。第二轮保持同一签名字节与期限独立重跑，原失败复现；第三轮只在原网关记录原生 I/O 阶段，明确 **response_headers／3,001 ms／未收到 HTTP 响应头**。设备 listener 总期限仍 2,000 ms、网关 socket 仍 3 s，未增加请求重发或延长期限。设备 USB 失败只读状态继续报告 Wi-Fi／MQTT／FRP ready；尚不能证明请求已到达设备监听器，不能归因于堆、打印或 FRP 单一实现。

三轮完整任务快照分别 **62／52／59**，对应历史堆最低 **150,376／147,312／150,216 B**、连续块均 **114,688 B**、控制栈 **2,800／2,888／2,800 B**、已观察任务栈 **1,196／1,260／1,196 B**。队列高水及处理中均 0、未发生产品申请或 FULL，连续块的入队后观察为 null；产品尚未运行，这些读数不是五能力容量。完整合法峰值、事件与 OTA 重叠资格和节省仍为 0／未取得。

三轮原 finally 擦实验数据，只恢复 fresh 独读原三码并逐字节回读；Root 再核对恢复字节与三码及对应新鲜全片一致，Wi-Fi down、串口释放、已构造的两夹具 close 和清理错误为空。运行中所属 PID／监听绑定没有在清理前捕获，不补写独立逐进程释放资格；HTTPS 产品夹具尚未构造。首轮原收据缺单独产品观察 bool，实际冻结 manifest 与 boot 绑定日志明确包含观察；后继准备器只修正该声明，原首轮收据不改写。

受限、Git 忽略的 `c3-validation-20261005/capacity16-product-queue-peak-software` 已存 **34,658** 个成员（固定 SDK **31,468** 项），归档 SHA-256 `a67334175cf0dd0e0c4c4cc5d8e70f524a751ff35fbd6d75bf156b33e23e251c`，索引 `2ea3c600551fe90a0ef39b038e4ab9af0c95a2004fdee1bb2b709ac47748ef19`。三个失败完整 runtime 各 **141** 文件＋1 索引；前两档 SHA 为 `f0924e4cf36a49cfef5ef81051e0872d1322c619759d77d24e243ffb387900b1`／`a878181e35e3caad5448b5f24771776b01167f9b2d61479c5c523c1359e48f4b`，分别位于 `capacity16-queue-observer-frp-preflight-failure`／`capacity16-queue-observer-frp-preflight-second-failure`；第三档 SHA 为 `0cbb8792356039d9680cea9734af0e27adc439ff14b9c61a1c4d7605c5071f03`、索引 `d3d464a1bf5f049e4ec02f7bff0b90397e0ad908fa9a4953a00a2008130681a8`，位于 `capacity16-queue-observer-frp-header-diagnostic-failure`。各档逐成员 SHA／size／mode／集合及原现场前后不变核对通过，失败不被后继重跑覆盖。

下一步只为重现的 FRP 首查记录设备监听器 accept／recv／send 与关闭阶段，继续原期限与原恢复合同，先证明数据面到达及返回；不把 ready 当认证管理通过。完整合法满队列、MQTT 槽／在途／outbox、最大 FRP／双流与 OTA 相遇、全部 native／分域申请／任务栈、Flash 时延／寿命、实体百次、ESP32、断电／72 小时与正式交付保持开放。

## 事件准入修正后的联合实体复验

✅ 只覆盖该行明确命名的源码、软件回归或 C3 有限切片；历史失败原样保留，容量、双板与总验收分别保持未完成。

2026-10-04，使用 Base `96d60df59160f1d7f5cef116f4dbf38a8f93911e` 的产品事件准入修正及本页所列新 A/C 实际完整构建；两轮均重新核对唯一 C3、两份一致 4 MiB、独读原三码，完整写入 A 后经 Wi-Fi／MQTT／FRP、公开安装、来源单次 3893 B 最大事件及唯一 WRITE 联合 OTA 到达持久确认。最大事件在 OTA 前已执行完成，不证明并存峰值。

首轮 driver exit 1，原 `passed=false`、`cleanup_incomplete` 保留。发布器先返回 120，其根因未知；之后收据及部分清理日志明确 ENOSPC。原下载过滤得到 149/149 MQTT／FRP ready，状态采样最低空闲堆 26540 B，来源 boot 历史最低 19820 B；22 条 TLS 记录与当前有限 PC 消费者的 131 个区间仅取得局部资格。原擦除／三码写回与 Wi-Fi down 回执已经执行，Root 再独立读回当轮 fresh 三码一致，没有额外 Flash／eFuse 写入；补证不改写首轮恢复收据中的失败字段。58 成员选中归档及六份材料已受限私存，私存索引 `844e33d69a4af707c5c996f430c6f48399cb6b394a2768fd1f9e7de788cfbff5`。首轮未完成后续生命周期、第三 boot、十二项业务、卸载及最终 A/C 读回。

第二轮 driver exit 1，UART 观察器的有界队列失败在 MQTT 停止／启动后的 USB product.status 查询中暴露。原始日志 634379 B＝已消费 568837 B＋仍待消费 65498 B＋拒绝入队整块 44 B；该块使 65536 B 上限实际超过 6 B。原始字节先写日志再尝试入队，没有删旧队列或当作串口断连重开。USB 停止／启动完整一轮、MQTT 停止及启动原 ID 的成功结果已保存；其后查询失败，第三 boot、十二项业务、卸载与最终 A/C 读回未完成。原下载过滤得到 147/147 网络 ready，状态采样最低空闲堆 30640 B，来源 boot 历史最低 19268 B；1 条合格 TLS 记录和 6 个 PC 区间仅局部通过。原三码精确恢复、擦除实验数据、Wi-Fi down、串口释放及原 fixture close 均通过，清理错误为空；末尾对 `passed` 的失败断言保留整轮失败，不是新增恢复缺口。37 成员选中归档及三份材料已私存，索引 `38b7ea906d8f2ed00d1927dbbb305e8c53750f1ff01a9ddcb37826a38416d6bb`。

以上历史最低水位不是下载阶段局部因果测量；采样最低空闲堆也不是完整峰值。新签名标签不继承旧 IRQ 资格，当前有限 SOURCE/PC 消费者不证明完整 IRQ、全栈峰值、largest 或 native 满峰值。两轮容量均未通过，新增实测节省仍为 0；不将两轮与旧镜像相减推算收益。

- [x] ✅ 宿主观察器边界与 27 项纯测试：当前宿主观察器只将两处 ContinuousUART 调用显式设为 1048576 B，并同步 scope。类及默认 65536 B 保持，溢出仍 fatal，不丢旧队列；设备预算、A/C 镜像、CLI 写序、业务、期限和物理恢复体不变。原 24 项纯测试与新增三项均 exit 0；本轮全部 634379 B 原日志零前台消费重放后 log/FIFO/字节一致，1 MiB 恰满再加 1 B 仍 fatal、旧 FIFO 保留，单 reader 和 join 通过。当前三个差异逆向后与前轮 driver 字节／AST 一致；不继承未重跑的历史 driver contract 全量资格。第三轮已独立完成以下功能切片；纯测试自身仍不提高实体或容量资格。

- [x] ✅ 本轮 C3 联合功能切片：第三轮真正 driver exit 0：公开一次 WRITE 联合 OTA、USB／MQTT 停止启动、五个原生命周期 ID 各 application 写一次及只读重查、停止后 MQTT 重启自动运行 confirmed 产品、旧 stop ID 在新 boot 经两入口均 unknown、十二项业务（含预期暂停拒绝）、卸载，以及来源 A／目标 C 逐字节读回全部通过。源码仍绑定公开 Base `96d60df5` 及同一新 A/C 签名字节，没有因宿主预算变化重签设备镜像。

原下载过滤 150/150 MQTT／FRP ready；来源 boot 历史最低 21360 B、状态采样 current 最低 31204 B。历史水位距离 49152 B 门差 27792 B，不是阶段局部因果、完整 native 峰值或实测节省。15 条 TLS 记录、drop 0、errors 0、qualification true；保存 UART 严格重解析与原收据整个 observation 相等，当前有限 SOURCE/PC 后置门通过 89 个区间。完整 IRQ／全栈、largest、满队列与 processing、最大事件及 OTA 并存、联合 Flash 争用仍无资格。

连续 UART 共 770858 B＝已消费 769644 B＋结尾待消费 1214 B，overflow 0、reader error 为空、reader 已退出且 join 无超时。本轮最大 pending 63737 B，低于旧 65536 B 门，因此不能将本次成功唯一归因于 1 MiB；前轮积压事实及新预算的严格边界测试继续保留。两份 fresh 原现场只互相比对及匹配独读三码；待刷入 A 候选与写后读回另行核对。首次离线归档脚本误将原现场与候选相等比较，明确拒绝后在新目录修正，首失败脚本及 JSON 原样保留；不是实体失败或重新执行设备测试。

- [x] ✅ 本轮 C3 恢复、资源释放与证据归档：原三码精确恢复、实验数据擦除、原 boot Wi-Fi down 及 UART 释放通过；Root 对实际运行中观测的三个所属进程／四个本地监听逐项核对释放，不作全局零进程声明。213 个完整 runtime regular 文件及必要外部材料组成 222 个 payload、223 个 tar 成员，原现场前后索引不变、逐成员 SHA／size／mode／执行位一致，有限独核 must_fix 为空。单档 SHA `d6ed4edefebac4d6558eb19b8cf6b58dd382d3bab3585a0bbdbbb360e0d6f61e`，选中索引 `a90b7c20ebd22d5a0c73ba5526d7f557f567fbc2e17e9d90cb601b810057c21e`；四份材料受限私存索引 `a0d97fea05e00bc0f98bbd9608a4b599b5dbeb1ca06c446ba219a11a05364e0f`。这是当前 C3 联合功能切片通过，五仓完整目标及容量保持未完成。

- [ ] 容量与完整交付：最新来源历史最低 21360 B 距离 49152 B 门槛差 27792 B，当前没有已验证的进一步容量优化路径；前两失败轮历史读数各保留原范围，不将同镜像不同轮次相减为收益。当轮按计划第 12.10 节请求维护者决定硬件／功能约束，并在等待期间保持原 48 KiB 等合同；现已由 2026-10-05 双目标统一 16,384 B 的明确决定替代等待，历史失败不改判，现行合同与新鲜轮见页首。满合法队列与 processing、largest、联合 Flash 争用、寿命 Y/f、ESP32、断电／72 小时及正式交付保持开放。

## 产品事件先准入后分配

- [x] ✅ 产品事件准入源码修正：2026-10-04，真实 `esp_base_container_product_offer_event` 保留参数和初始UNAVAILABLE检查，在既有FIFO锁内核准当前包、准入状态及空位后才malloc／copy。BUSY／INVALID／FULL先于NO_MEMORY硬切；复制后仍检查atomic停止标志，取消副本解锁后清零释放。生产MQTT调用者只消费ACCEPTED，拒绝不推进序号；队列上限、签名包、guest、停止状态和预算保持。该修改消除拒绝路径额外临时owner：合法3893 B MQTT guest载荷加48 B头的旧申请界为3941 B，不能算实测堆收益或闭合约29 KiB容量差。

- [x] ✅ 双目标 host／真实签名 guest 软件回归：真实offer生命周期新增八槽全满＋一个处理中、零申请拒绝、准入OOM释放锁、最大载荷、复制中取消的清零／单次释放、旧包隔离；MQTT回归核拒绝后原序号可重试且不允许跳序。C3／ESP32普通full host与真实Container／WAMR签名guest完整入口均exit0，ASan／UBSan、各百次生命周期及Darwin资源检查通过。首次Darwin测试memcpy宏冲突导致末编译单元exit1，修正仅测试include作用域后两完整入口重跑通过，失败日志与收据保留。14份选中源码／结果／日志／收据已私存，索引 `a2bf714a6e66816163b4ded72b7631380aaa14bb39490932cbf0e1567f17f7ed`。

- [x] ✅ 新 A/C 构建、验签与输入核对（软件范围）：生产源摘要 `85dabcd0c58e4508414b31c4716b7c5a3aebb821e99d8372f77d8ebb2519466a`；新A/C完整SDK构建及官方RSA验签通过，版本 `0.2.0-c3-event-admission-a/c`，signed各1183744 B；34558实际源／31468SDK核通过，仅该函数、两装配路径和生成锁摘要变化，SDK零差异。实际offer代码增加32 B、局部frame保持48 B，BSS、rodata、IRAM和堆起点保持；不能据此增加整体可用堆或证明全调用栈。分配／复制延长已有锁持有区间，最长持锁与MCU调度影响未测；没有新增锁或状态。软件只证明拒绝少一个owner，actual_saving_bytes=0、capacity=false；完整合法峰值、largest连续块和联合OTA／Flash争用仍待实测。

## 相同 Wi-Fi 配置的生命周期修正

- [x] ✅ 相同 Wi-Fi 配置生命周期源码修正：2026-10-03，真实 Wi-Fi owner 在输入校验后、状态修改前比较 configured／SSID／密码。相同选项且 station 正常运行时，保留已建立的连接证明、连接尝试和重连退避；稳定未配置状态保持。真实字段变化仍停止后重新配置，failed／尚未正常启动仍恢复，停止中接受最新选择且不延长原期限。没有新增状态、任务、缓冲或持久键。

- [x] ✅ Wi-Fi 生命周期与双目标 host 回归：包含真实 runtime 与配置校验器的 ASan／UBSan 12 个启动阶段全部通过，新增生命周期序列覆盖三次同配置提交、连接中与退避保持、SSID／密码／configured 变化、回滚、停止中最新选择、旧 IP 事件、队列溢出、停止错误和原停机期限。C3／ESP32 两个原 host 入口在公开源码与已锁定依赖的独立装配上均 exit 0。直接公开入口缺依赖和首版测试把无效尾部误当有效配置的失败均保留；没有把这些失败改写为通过。有限源码审查 must_fix 为空。公开 Base 已保存推送 `3e8e44e9723fec4a7aaaa334ce9cd7c70cec2eec`；新 A/C 已完成完整 SDK 构建、官方验签与下节有限实体复验，容量与唯一根因资格保持未取得。

## 修正后的最大记录与自然退出切片

- [x] ✅ 第五轮 A/C 构建与验签（软件范围）：2026-10-04，第五轮以公开 Wi-Fi 修正为唯一新执行变更，继承第四轮实际构建的原正式预算与观察器。新版本 `0.2.0-c3-wifi-equal-a/c` 两个签名镜像各1183744 B；完整 SDK 构建与官方 RSA 验签通过。实际源核对仅 Wi-Fi 源、两处绝对装配路径与生成锁摘要变化，SDK源码零差异。A/C的FLASH code各增加160 B，rodata、IRAM、data、BSS及堆起点不变；Wi-Fi配置函数局部frame增加16 B，不据此宣称完整栈、IRQ或容量资格。来源A摘要 `5c87a51e3c3be4641162427d81a5e507d8248a5a7cd25879a19caa2404bbd563`。

- [x] ✅ 第五轮 C3 最大记录／单 Pong／公开移除切片：重新取得两份一致4MiB和独读原三码，新A完整写入／读回、实际启动版本与摘要、空产品绑定通过。最大合法65536 B控制记录、同boot唯一Pong以及公开移除均成立。移除ACK为succeeded／revision3，FRP快照ready、uptime50748ms；27ms后唯一后续status为unconfigured，共两条revision3回复。158条命令＝154status＋3config.set＋1product.status；不继承第四轮80次status。

- [x] ✅ 第五轮 C3 原清理前自然退出门：**driver exit0、原清理前自然退出门通过、physical_measurement_qualified=true仅属于本C3切片。** 设备明确STOP ticket1／error-19时仍持有原connection，fd53 close返回0；两次日志采样差3258us含UART和调度，成功时errno11为陈旧值。Host raw Read自然EOF，await／mux／device返回时context为none且signal／deadline均false；最后普通defer取消晚于device_return。await观察4828424us含移除前等待，不是公开停止耗时。修正后结果与去掉同Wi-Fi重启的链路一致；没有TCP FIN／RST包级交付或唯一根因证明。

- [x] ✅ 第五轮 C3 有限 Flash 调用测量：最大记录wall **2544305us**；整区erase1次／65536 B／**591431us**；write68次／65536 B／累计328730us／单次最大10774us；read2435次／1179648 B／累计174923us／最大2671us，native失败0。boot recover另scope：wall540538us、erase540467us。调用计时包含锁／调度，不能当纯Flash、不可抢占或最坏上界。UART **126504 B＝126504 B＋0 B**，overflow0、read error为空、join正常，stderr为空。

- [x] ✅ 第五轮 C3 恢复、释放与原档核对：结束擦全部实验数据，只恢复当轮fresh三码并逐字节读回；原boot Wi-Fi down ACK、串口和已绑定fixture PID／监听释放由Root直接核实，只声明所属资源。完整 **77 regular成员**归档摘要 `c8781fb32bebfbe594b64eefb2dfe5cf09c8ba67b3f10a03d937b29458afe47d`，私存 `c3-validation-20261003/c3-wifi-equal-physical` 六材料索引 `071cffc3508a607047baeec245dd7405310172ae9321f3d3d270d86b2c0d6dd1`。全77成员有限独核通过、must_fix为空；四成员审查归档摘要 `00edd2c9b47b4f1b8e03abe2192df2194cef1b196afc42ef4dc8360bce34d4bf`，三份私存索引 `89f45327b610d5a7e93bbfde77f9a002a909c9a7cc4d7e4cfa3ba31919630b1e`。审查只核原档，不新增测试或设备操作。前四轮失败保留原资格。该轮没有MQTT、guest或联合OTA，minheap148464 B不能外推五能力；capacity=false、联合争用／峰值／寿命=false、实际新增节省0。

下一修改只处理已有产品事件准入：在现有锁内核准包与队列空间后再分配，保留复制后停止复核及失败清零释放；真实双目标host与签名guest生命周期、MQTT序号回归已通过，新A/C SDK／验签完成，联合实板继续，不提前记容量收益。停止策略继续为仅当前启动停止、重启自动运行confirmed产品。五能力容量、最大合法峰值／native、联合Flash争用、寿命Y/f、ESP32、断电／72小时及正式交付继续开放。

## 原生关闭与同配置 Wi-Fi 重启

2026-10-03，第四轮在原正式预算上，仅仓外增加 C 客户端既有对象、fd、停止 ticket、原 close 返回和即时 errno 的日志。关闭策略、任务、缓冲及期限保持。新的 A/C 完整 SDK 构建与官方 RSA 验签通过；版本 `0.2.0-c3-native-close-a/c`，两个签名镜像各 1183744 B，来源 A 摘要 `85b79e52296ec6c25ae1ab4a0e7c9edd788b366840cce2179428ac67ced7c0f4`。34558 项实际源核对通过，31468 项 SDK 零差异；仅两处 FRP 日志源、两处绝对装配路径与生成的锁摘要变化。实际 ELF code 增加 152 B、rodata 增加 288 B，BSS／IRAM／堆起点不变；两个关闭局部 frame 各增加 32 B，不能据此宣称完整栈或 IRQ 资格。源码、实际构建及 54 个冷输入成员的有限独核通过。驱动只更新镜像身份，不继承新镜像未执行的纯测试或旧 PC 地址资格。

新实体重新读取两份一致 4 MiB 与三个独立原代码区，完整写入／读回、实际启动摘要和空产品绑定通过。最大合法 64 KiB 记录及同 boot 唯一 Pong 成立。record wall **2568064 us**，erase 1 次／590914 us，write 68 次／65536 B／累计 358809 us，read 2435 次／1179648 B／累计 175487 us，原生失败数 0；boot recover 另 scope：wall 572725 us、erase 572654 us。这些有限调用计时包含锁与调度影响，不是纯 Flash、不可抢占或最坏上界。

**整轮 exit 1、physical_measurement_qualified=false。** 原自然退出检查循环再次失败。移除 ACK 为 succeeded／revision 3，FRP 快照 `network_unavailable`；随后 80 次 status 全为 unconfigured，共 81 个 revision 3 回复。211 条命令＝status 207＋config.set 3＋product.status 1。同设备日志中 fd 53 的 close 返回 0，两个采样差 943 us 包含日志与调度，成功时 errno 113 是陈旧值。随后 ticket 0／error -17 仍关联原连接，明确 STOP 的 ticket 1／error -19 到达时连接已为空。不能把前次 close 称为本次显式 STOP 调用，也不能以 close 0 证明对端 FIN／RST 交付。

Host 九条阶段日志仍在 SIGTERM 取消后才出现关键返回和 FINISHED；awaitStop 观察 13502058 us 包含移除前等待，不是设备 stop 耗时。原成功边界门未执行，清理后纯重解析拒绝自然资格。连续 UART **160185 B＝159239 B＋946 B**，无溢出／读取错误，join 正常。结束擦实验数据，仅恢复本轮 fresh 三码并读回；原 boot Wi-Fi down ACK、串口、实际唯一夹具 PID／监听释放经 Root 直接核对。stderr 中 cleanup 最终要求 passed 的异常属于整轮功能失败，不能据此否定已经核实的恢复。

完整 **78 个 regular 成员**归档摘要 `c9b4c1ecf62093d8645edc12c03cd63fb917c82e13eede656d92e270b4896d19`，私有保存目录 `c3-validation-20261003/c3-frp-native-close-failure` 的六份材料索引 `a8f2556a25168b492b6a1728852753625e2b4f38cbdb14fb762a8f7b7aea8b59`。当前失败全78成员有限独核通过，must_fix为空；四成员审查归档a2ad93cc71182e2048a785f6d84d32d72103535ca7ab532b89b59171a7a0e51f及三份拷贝已私存，索引7fc4a2d491eacf2ab303f909e2d26c35fb4151f2d2fdb9d2d5efb383237bc9ef。原失败均保持；恢复和审查通过不提升整轮资格。

后续只读核查确认，三次请求的 Wi-Fi 字段完全相同；Base 完整 config.set 仍先无条件停止 Wi-Fi，待重新建立连接并提交配置后才重配 MQTT／FRP。源码路径与本轮 ticket 0 网络错误先于 ticket 1 明确停止的日志一致，尚未证明远端超时根因。后继已修正 Wi-Fi owner 的相同配置语义并通过宿主检查，见本页首节；正常连接或重试保持原状态，字段变化、失败恢复、停止中最新选择和真实连接证明仍由原流程处理。不增加期限、状态机或关闭策略。以上结论属于修复前第四失败轮；修复后的SDK／有限实体资格见本页首节，不改变该失败轮原资格。

停止产品策略已确认：仅当前启动停止，重启自动运行 confirmed 产品，不持久化停止状态。新增节省仍 **0 B**；容量、满合法峰值／native、联合 Flash 争用、寿命 Y/f、ESP32、断电／72 小时和正式交付继续开放。

## FRP 自然退出阶段复验

2026-10-03，继承下述同一签名来源 A、实际 SDK／ELF、正式资源和 80 B Flash 观察成本，仅更新仓外服务端阶段观测及 Python 消费者。Go 新程序仍使用原认证、单条 65536 B 控制记录、15 个 ReqWorkConn 和唯一 Pong，不改变原 `awaitStop`／`mux.Close` 弃错行为；新增日志只含错误分类、单调时间与取消原因。原 16 项端口检查、8 项 race 检查、两次真实 native 回环及有限独审通过；回环没有复现 C3 问题，不能当实板原因。Python 当前 36 项纯验证和最终增量独审通过；父 30 项报告仅为历史。70 个后继归档成员及额外冻结收据独核通过，没有重新编译固件或 SDK。

驱动保持单次 90 秒 fresh-boot capture、原 5 秒 status 与原 10 秒自然退出检查循环。在公开 unconfigured、进程退出 0／FINISHED 后、清理之前，再要求三个关键返回阶段完整且唯一；拒绝信号、截止时间和底层读超时。正常返回后的普通 defer 取消可以保留。该循环涉及状态查询，不把循环参数当严格硬实时上界；解析失败仍进入原恢复流程。

第三轮实物重新读取双新鲜一致 4 MiB 和三个独立代码区，来源 A 完整写入／读回、实际启动摘要和空产品绑定通过。只配置 Wi-Fi／FRP；最大合法记录及同 boot 单 Pong 成立。移除 ACK 成功、revision 3，其 FRP 快照暂为 `network_unavailable`（uptime 47566 ms）；18 ms 后首次 status 开始，**80 次 status 全为 unconfigured**，总 81 个回复＝ACK 1＋status 80。本轮 212 条命令＝status 208＋config.set 3＋product.status 1，不继承前轮 84 次查询或 ACK 快照。

本轮成本为：最大记录 wall **2578796 us**；整区 erase 1 次／**619128 us**；write 68 次／65536 B／累计 332715 us／单次最大 13858 us；read 2435 次／1179648 B／累计 174220 us／单次最大 1527 us；原生失败数 0。boot recover 另 scope，erase 552684 us、wall 552753 us。调用计时可能包含锁和调度等待，wall 还含非原生开销；两个有限实板样本不构成纯 Flash、密码学、不可抢占或最坏时长，以及联合争用／寿命证据。

**整轮 exit 1、physical_measurement_qualified=false。** 原退出循环未观察到自然结束；Go `await_stop_enter` 至返回观察持续 15118291 us，包含移除前等待，不能称设备 stop 耗时。该轮先记录 `context_done=signal_sigterm`，随后 raw Read 才返回 `connection_closed`，再出现 awaitStop／mux.Close 返回、FINISHED 及仍带 SIGTERM 的 `device_return_context`。这证明当前日志的阶段／取消边界，不能证明设备 fd、FIN／RST 包或网络交付失败原因，也不回写为旧轮取消因果。清理后纯重解析拒绝自然资格；原实体驱动因循环超时没有进入成功边界内的 pre-close 门。仅本地关闭成功不能替代对端终止证据，下一步继续限定设备侧关闭链路核查，不延门或放行。

连续 UART **160230 B＝159247 B＋983 B**，无溢出／reader 错误，线程 join 正常。结束擦除全部实验数据，只恢复本轮 fresh bootloader／partition／factory 并读回；原 boot Wi-Fi down ACK、串口及实际唯一夹具 PID／监听释放由 Root 核对。完整 **90 个 regular 成员**归档摘要 `d2a0bdee6f3201039d34ea3c73035a20c404c9c5ecd4b6dac01828a4ad9540af`，受限且 Git 忽略的 `c3-validation-20261003/c3-frp-flash-stop-diagnostic-failure` 保存六份材料，私有索引 `2f8e3a7433503e07576a006b345d51e74f470666df8036b9efb154c8065c8d75`。前两次失败完整保持；当前实板全90成员有限独核通过、must_fix为空，六成员审查归档12152955d5d9050faab9a17611b07e0d65510488068a3683dd50aca8820aa5c1及三份审查拷贝已私存，索引f6ea2222b05bfb915a735e3a01b1e0609dc8006dde638f101aca59adc1471f1e；恢复与分项核对不提升整轮资格。

有限32选中源／配置与17实际编译单元、11个来源A完整函数机器码只读追踪未发现work池丢control fd的路径。实际A仅在native close返回0后清fd，TLS／session借用control connection；但本轮没有具体fd／close返回实测。lwIP零linger在有unsent／unacked时才abort，其他分支仍可等待FIN后续输出；源码允许tcp_output错误被忽略、FIN分配不足后等待timer重试。这些只是待区分路径，不能指定为本轮根因。后继原生关闭观察与同配置 Wi-Fi 核查见本页首节；没有增加任务、缓冲、关闭策略或延长门。只读六成员审查及三份拷贝已私存，索引c53cbbc2ba050bafcba9d98248304f8293f618e3dc200a054f975d4de5d6996c。

产品停止策略保持已确认的“仅当前启动停止，重启自动运行 confirmed 产品”，不增加持久状态。实际新增节省仍 0 B；五能力容量、完整峰值／native、MQTT／OTA Flash 争用、寿命 Y/f、ESP32、断电／72 小时和正式交付仍未完成。

## FRP 最大控制记录实测与移除超时

2026-10-03，在下述公开运行源码、精确组件与原正式预算上，仓外叠加 Flash 成本观察器。新来源 A／目标 C 完整 SDK 构建及原测试键官方验签通过，版本为 `0.2.0-c3-flash-cost-a/c`，签名镜像各 1183744 B；目标 C 此次只有软件构建资格。唯一 provider 状态由 64 B 增至 144 B，实际 BSS 和堆起点增加 **80 B**；IRAM 50106 B＋70 B 对齐保持，观察成本不加回。34558 项实际源构建后核对通过；两 provider、两绝对装配路径及 Component Manager 生成的锁 `manifest_hash` 为五项变化，依赖版本和其余源／SDK 字节保持，新实际 ELF 和 PC 重新绑定。

最大合法 65536 B 控制记录先经真实 host TLS／Yamux／原生 session 和 IDF Flash 模型消费、坏认证拒绝；这部分仍是软件。严格 decoder 25 项、第二轮 driver 30 项、adapter 10 项及单次启动 capture 15 项纯检查通过；当前第三轮消费者见本页首段。最初实物轮省略已验证的 pyserial fresh-boot capture，首 `status` 超时、运行 UART 为 0 B；整轮失败及 79 个完整成员独立保留，后继补回一次 90 秒真实 READY／empty 捕获，关闭后才打开 POSIX 单 reader，原五秒状态查询与恢复逻辑保持。新后继输入包 53 个明确成员核对通过，没有复用失败基线。

第二轮重新读取两份一致 4 MiB 基线和三个独立原代码区，完整写入／读回后实际启动来源 A，公开查询绑定当前固件摘要与空产品。只配置 Wi-Fi 和 FRP，无 MQTT、OTA 或 guest。一个合法最大记录随后到达同 boot、同配置 revision 的唯一 Pong。实际原生观察如下；单位为微秒，调用数与字节均为本轮实测，不能填入其它分段形状。

| 最大记录项目 | 原生调用数 | 申请／读取字节 | 原生累计耗时 us | 原生单次最大耗时 us |
| --- | ---: | ---: | ---: | ---: |
| 整区擦除 | 1 | 每次 65536 | 564051 | 564051 |
| 写入 | 68 | 65536 | 348150 | 11076 |
| 读取（含回验） | 2435 | 1179648 | 175497 | 1248 |

最大记录 `begin_to_clear` 的 `wall_us=2565029`、`result=0`、`cost_valid=1`、原生失败数为 0；boot recover 另一次擦除为 554330 us、wall 为 554399 us。wall 结束于 guard 解锁后、打印前采样，包含调度与非原生开销，不是独立密码学耗时、整个回调耗时、连续不可抢占时长或最坏上界；500 ms claim 重试也不构成该上界。两项观察不含实验 ROM 整片擦除／恢复磨损。

公开移除配置 revision 3 成功，同 boot 之后 **84 次只读 status 均为 unconfigured**；另有一次移除 ACK，共 85 个 revision 3 回复。driver 未在十秒联合门观察到夹具自然退出，因此 **整轮 exit 1、physical_measurement_qualified=false**，没有放行成本总体资格。Go 最终日志含 FINISHED、退出 0，但没有事件时间戳或实际 SIGTERM 发送记录，只能说明 cleanup 后最终收据看到这些结果，不能证明精确发生先后或取消因果。同精确夹具与原生客户端的两次本机认证联调均自然退出，没有复现 C3 超时；后继关闭阶段证据见本页首段，不延长或削弱退出门。

启动 UART 18111 B 与保存摘要一致；运行 UART **162412 B＝consumed 162348 B＋pending 64 B**，无 reader 错误或溢出。结束擦除全部实验数据，仅恢复本轮原 bootloader／partition／factory并逐字节读回；原 boot Wi-Fi down ACK、串口释放、实际夹具 PID／唯一 LAN listener 存活时绑定及最终释放由 Root 直接核对。完整失败现场 85 个成员归档摘要 `3e276ba3f8c6bb0df2def063780c4fcdea65975b133c772724b194f19a115d06`，受限且 Git 忽略的 `c3-validation-20261003/c3-frp-flash-cost-removal-deadline-failure` 保存六份拷贝／澄清，私有索引 `2b55404e152abe0fc19ff57464fe0507b5e35eff6533123bd39d35e9154ba56c`；原首次失败与所有旧轮独立保持。

源码和当前官方普通控制链确认，正常 Pong 是 8 B、4 B 两条短记录，现有独占 4096 B 窗口已在完整认证后消费，**没有 scratch begin／erase**。无失败主路径的每扇区磨损按实际 boot recover 次数 B 与大记录 begin 次数 L 计，不能按十五秒心跳直接推擦除率，也不能将整区十六个 sector再乘作每扇区次数。Y／大记录频率 f 与实物已有磨损仍未确认，本次不计算剩余寿命。实际新增节省仍 **0 B**，没有联合 OTA Flash 争用、五能力容量或完整最坏成本资格；P6-03、满合法峰值／native、ESP32、断电／72 小时与正式交付继续开放。

## 同池共享状态与完整 TLS 实体诊断

2026-10-03，在下述同一公开运行源码与精确组件版本上，仓外诊断将固定 **1800 B** 观察区改为逐记录20 B与精确状态77 B共享。固定部分344 B、共享池1456 B，容量条件为 `20×N + 77×D <= 1456`；68条只适用于一个状态，不承诺任意68次新低都能保存。新低不被过滤，身份、释放回填、歧义／ISR／溢出失败及原V2字段保持。25组host、ASan/UBSan、两C3单元和独立多身份回填反例通过；正式Wi-Fi RX／TX 32／32、TLS 16384／4096、64 KiB guest、任务栈和协议上限保持。

实际来源A／目标C完整SDK构建及原测试键官方验签通过，版本为 `0.2.0-c3-tls-share-a/c`，签名镜像各1183744 B。IRAM50106 B加70 B对齐，堆起点 `3fc999a0` 较15槽诊断后移1024 B；history最终单函数帧96 B，不能当整链峰栈。唯一1800 B状态、六个SOURCE位置、allocator RA、IRQ闭包与实际双ELF核对；新PC表摘要 `949a2bad0c4691a596d6dd763882c0ec509265c4a15bb832bd0d3cd5b9bd5865`。全部34558实际源及实际编译输入重新绑定；观察成本不加回，实际节省为0。

首版34895项冷输入全部字节与归档完整，但生成门模板仍固定旧parser摘要，实际加载拒绝、完整准入失败，未上板。后继在新driver硬切parser／schema和模板摘要，83解析恢复＋18PC测试以及实际生成门加载通过；34903项完整冷输入（34628源／275证据）与两份归档独核通过，冷索引 `016de45008c5ecb87bcb5951a10c097717cce4c885495d9914f34ecfdd7105b5`。固件和SDK没有因这个消费修正重新编译。Root首命令误从运行目录副本启动，被原路径守卫在串口前拒绝；失败保留，后继使用全新目录和原冻结入口，未修改守卫或沿用失败基线。

本轮双新鲜4 MiB基线与三个独立原代码区一致。公开最大事件、原一次WRITE联合OTA持久确认、USB／认证MQTT各一次停止／启动、五原生命周期ID各写一次及只读重查、停止后重启自动运行当前固件confirmed、旧stop ID跨boot unknown、十二项业务、卸载及A/C逐字节读回通过。停止仅当前启动有效，不增加持久停止状态。完整driver实际退出0，TLS **20条、drop0、errors0、qualification=true**；保存UART严格重解析与收据相等，实际ELF后置门通过119个符合条件区间，输入前后稳定。

来源下载 **147/147** 状态MQTT／FRP ready，历史minimum **19628 B < 49152 B**，20条解锁后快照中的最低current为20204 B。不同轮独立读数不相减归因，不能把解锁后快照升格原生最低堆时刻的消费者或因果证明，也不证明最大FRP记录、满队列／在途、并存事件或连续块门。458个完整任务快照、13个已观察名称栈余量至少1024 B，仅覆盖实际观察范围。UART logged727237 B＝consumed726023 B＋pending1214 B，最大队列63707／65536 B，无溢出；不称结束pending为零。

结束整片擦除实验数据，仅恢复本轮独立读取的原bootloader／partition／factory并逐字节核对；reset、Wi-Fi down ACK、串口、三fixture与所属进程释放核验通过。FRPS服务与remote proxy端口在存活时直接记录，结束对五个实际listener逐项核验无监听，不恢复旧／实验NVS，不写eFuse。完整现场归档 **206文件／11目录、217成员**，摘要 `f846fade2b04cf933fa5e84ce128a2b77d69b044824d02b95e69ea18b4ef053d`；包含10个实际生成pyc输出，不称源输入或零缓存。首次归档过严“无pyc”断言失败保留，后继完整归档不删除输出。受限且Git忽略的 `c3-validation-20261003/c3-tls-owner-shared-physical` 保存完整归档和11份选中拷贝，私有索引 `4a1dc9a6091cbcdb0d957359c88e4d53dd82d0f972772c79009f726e7e54f705`；完整实体独核及21成员审查归档通过，审查归档摘要 `3392116acffcb41edbc5fced77419ea74bfbeffd651f0062bfa74fcf7d9e309c`，最终审查私存索引 `d2cd0d83e28fca387fdd2f45e8e329479e26513c4d67942f5fcb25432f0f0284`；旧失败不被覆盖。

当前RX生命周期只读核查没有新准入收益：正常空闲已回到24 B，部分密文、未读明文和握手仍有真实消费者。当前confirmed代表Wasm没有data section，data_copy根本未分配，该方向可省0 B。Flash计时观察器两源增量、原5＋新7组host／ASan/UBSan和两C3单元、53选中成员独立代码审通过；该前序检查点仅软件，后继完整SDK与实物有限观察见本页首段。Base每实例DRAM增加80 B，wall截止于guard解锁后、printf前采样，可能含调度延迟，不能当精确解锁时刻、完整回调耗时或500ms操作上界。寿命Y/f仍待维护者目标，全部软件模拟不构成时延／寿命通过。

**完整TLS诊断、有限联合功能与恢复通过，容量仍失败。** P6-03、满合法峰值、native全生命周期、Flash实测及寿命目标、ESP32、断电／72小时和正式交付继续开放。

## 十五槽无损压缩与完整实体续验

2026-10-03，在下述同一公开运行源码与精确组件版本上，仅在仓外将固定 **1800 B** 观察区的记录从120 B无损压缩为96 B，共15槽。地址编码覆盖当前C3合法DRAM／IRAM／RTC地址；范围外值、序号耗尽、身份歧义、ISR及溢出仍使资格失效。原V2导出字段、实际SOURCE位置、精确分配身份和释放回填保持，未过滤新低或放宽parser。正式Wi-Fi RX／TX 32／32、TLS 16384／4096、64 KiB guest和任务栈预算保持，不提前停止旧guest。

实际来源A／目标C完整构建与原测试键官方验签通过，版本为`0.2.0-c3-tls15-a/c`，签名镜像各1183744 B，摘要分别为`97a5c5d2e74278479852e8b3ca36e71d4ae427cac2671730994dbda929c4434c`／`6a1313fcf5380be5ddb59750f96757bcb0b4c7674ed08868b88a205fcdd737d1`。完整冷输入34950项（34628源输入／322证据）和两份归档逐项核验，冷索引`369fbe656355fea6cdf8cf5c7bbc274e0c79fb43f971dbe6fe71d1d739b77da2`；实际双ELF及六个SOURCE位置重新绑定，未复用前序PC表。实际IRAM为49086 B加66 B对齐，堆起点较前序12槽诊断后移1024 B，全部计入观察成本，实际节省为0。SDK子模块自动初始化曾改变复制树的lwIP，完整源码守卫在编译前拒绝；失败副本独立保留，后继仅采用SDK已有的显式跳过检查入口，并重新核对全部SDK输入未变。

本轮重新读取双新鲜一致4 MiB基线和三个独立原代码区。来源最大公开事件、原一次WRITE联合OTA持久确认、USB／认证MQTT各一次stop/start、五原ID各写一次及只读重复、停止后重启自动运行confirmed、旧stop ID跨boot unknown、十二项业务、卸载及A/C逐字节读回通过。停止仅当前启动有效，不持久化停止状态。下载150/150状态MQTT／FRP ready，历史minimum **23620 B < 49152 B**；不能与前序23464 B相减归因，不加回成本，不证明满合法峰值或连续块门。

TLS探针尝试留存24个新低，实际保留15条、另9条丢弃；BEGIN／END结构完整，END为`qualified=0/errors=128`。原parser重解析与完整收据相等，保持`unqualified_observation`；重新实例化的实际ELF后置门退出2、eligible区间为0，未提升局部记录的owner或因果资格。Root实体driver会话75593实际退出1；独立报告没有落盘exit文件，诚实保留null，并另记录Root工具会话来源。**功能与恢复通过，整轮、TLS资格和容量均未通过。**

463个任务快照expected/captured相等，无采样申请失败；13个已观察名称栈余量至少1024 B，范围不扩大为全部native生命周期。UART logged与consumed均732176 B，pending为0，最高64967／65536 B，无溢出。结束擦除全部实验数据，仅恢复本轮独立读取的原bootloader／partition／factory并逐字节核对；reset、Wi-Fi down ACK、串口、三fixture及所属进程释放独核通过，不恢复旧／实验NVS，不写eFuse。四个已保存host listener直接复核无占用；FRP remote端口数字未保存，只由所属FRPS关闭及零所属进程限定绑定，不虚构第五端口直接实测。

189个runtime文件与10目录的完整快照归档摘要为`58327b137612d2f22d45725eb759d73ed6dad750d85ba3e94145369b9aced856`，独立审查归档为`71c2e9a7d7fce1c792850c0a71d6edb86b33d38b7f2d4ee4ae0345474d802ea2`；Root逐成员复核字节、大小、mode／执行位与集合。受限且Git忽略的`c3-validation-20261003/c3-tls-owner-history15-physical`保存22份精确拷贝及私有索引`26683226546d413a22490ce25a4f61807e035bd36c2fdc691033096ac0e4fbcd`，引用同级软件与冷输入审查检查点；前序所有失败保持独立。

容量owner与当前Wi-Fi IRAM限定只读复核均无新增节省：9825 B已识别native owner仍有真实消费者，1807 B／939 B方向尚未实施；64 KiB linear申请含8 B guard，不存在第二页。Wi-Fi本机IRAM／RX IRAM／EXTRA／SLP选项已关闭，相关已装载optional段实际位于Flash，不能再算一次迁移收益，IRAM／DRAM共享窗口也不能双算。后继仅研究同池精确状态共享是否值得实现，尚无实施或新容量通过结论。P6-03、满合法峰值、native全生命周期、Flash最坏实测与寿命目标、ESP32、断电／72小时和正式交付继续开放。

## TLS owner 完整软件冻结与实体历史溢出

2026-10-03，在 Base `daf9cd8d46bb22cad7f9cc6656de995a25e4fd89` 运行源码和 FRP `989cc876d92b815aeb0b6806fb861f0ee2b39a86`、MQTT `6443b71db761f4d667503f14108687bad5e6b5ee`、OTA `bf11916ab904be4ee9bcdfae213c85336363e96a`、Container `2b93b979b8b0760dcb96b28ac5d13fc52ae547bf` 的精确依赖上，完成仓外 C3 TLS owner 诊断组合。唯一固定记录区仍为 **1800 B**，替换旧探针；公开固件、依赖 pin、Wi-Fi 动态 RX/TX 32/32、TLS 16384/4096、任务栈与 guest 预算保持。私有 SDK 五处注入、四 provider 装配及全部五处实际 SDK guard 共 21 个精确增量；这是显式私有诊断覆盖，不是原 SDK 或生产固件原样构建。

实际 A/C 均完成完整构建和官方测试键签名，镜像各 1183744 B，原 0x130000 槽保留 61440 B。实际 ELF 的唯一 1800 B 状态、IRQ 路径与六个保留 SDK SOURCE 返回位置独立核验；第七处 object 调用被合法 linker GC，没有虚构地址。签名镜像、完整源清单、配置、ELF 与 PC map 精确绑定；C3 堆起点相对旧诊断后移 512 B，只计成本，不加回读数。两份实际配置除版本外相同，原资源和信任策略没有降配。完整冷输入 **46049 项（34625 源输入／11424 软件证据）**逐项及两份归档成员复核，索引摘要 `d8f7832551feeaa8a18ce37a22eff82b181c8536f5c5c9c1ca88123b486518eb`；原历史冷输入均保留。

准备失败各自保全：第一轮漏交付 Wi-Fi 配置和 FRP 控制 probe，只到空产品新固件启动，后续网络／OTA未执行，原三码恢复和数据擦除通过；补齐后新增 37 项必需输入及纯配置检查。第二轮在首写前第二次 4 MiB 基线读取中串口中断，零擦除／写入成立，另行只读复位成功；另一错误执行入口在设备调用前被路径守卫拒绝。三者不混入下述完整功能轮，也不修改原失败树或沿用失败基线。

新完整实体轮重新读取双新鲜一致 4 MiB 基线与三个独立原代码区。单次 4096 B 公开帧／3893 B 来源事件返回 3912，随后原一次 WRITE 联合 OTA 持久确认、USB／认证 MQTT 各一次 stop/start、五原生命周期 ID 各写一次及只读重查、停止后重启自动运行当前固件 confirmed、旧 stop ID 在新 boot unknown、十二项业务、卸载和 A/C 精确读回通过。停止只在当前启动有效，不增加持久停止状态。来源下载 **148/148** 样本 MQTT／FRP ready，status 采样历史 minimum **23464 B < 49152 B**；不与其它轮相减归因，不加回观察成本，不证明最大 FRP 记录、满队列／并存事件或连续块门。

TLS 原始 UART 的 BEGIN／END 和 12 条保留记录结构完整，但另 **3 次 probe 观察新低未保存**：BEGIN `dropped=3`，END `qualified=0/errors=128`。原严格 parser 与 Root 独立重解析完全一致，保持合法 `unqualified_observation`；后置 PC 门返回 2 并保全原对象，未把局部记录提升为完整 TLS owner 或最低堆因果证明。功能分项通过，**整轮 exit 1、TLS 资格与容量均未通过**。后继只能在同一 1800 B 内无损压缩记录，保留原新低触发和严格溢出失败；当前尚无新的编译或实体通过结论。

451 个完整序列化任务快照 expected/captured 相等，无采样申请失败；13 个已观察名称栈余量至少 1024 B，最低 IDLE 1196 B，仅覆盖实际观察范围。UART logged 722383 B = consumed 721169 B + pending 1214 B，最高有界队列 63234/65536 B，无溢出；不声明结束 pending 为零。结束擦除全部实验数据，只恢复本轮独立读取的原 bootloader／partition／factory 并逐字节核对，reset／Wi-Fi down ACK、串口、三 fixture 和零所属进程释放独立通过，不恢复旧／实验 NVS，不写 eFuse。15 组事实审查通过不等于整轮通过。

完整实体 **189 个成员**的字节、size、mode／执行位已核，归档摘要 `5ce3c33a02cf9f1a65cc6ccc5d7a5b0233f7463f4b2b8bc61416bc87468192d0`，索引 `f65f1b31d27fc6569ccd2f3653f29e795a8c05616aa8941a8abe5f9e0f5c5fd5`。受限且 Git 忽略的 `c3-validation-20261003/c3-tls-owner-history-overflow` 保存 23 份精确拷贝及索引 `5e51caba77b32f3c513e31502519eb0a847072e2bd966fb1ea69c2021c59ee98`；完整软件冷输入引用同级 `c3-tls-owner-actual-software` 的独立私有索引。首写前串口失败 85 个成员、两份 Flash 官方／源码成本归档及独立复核也单独保留。

### Flash 成本与寿命边界

只读源码核验确认，scratch 用于**入站加密控制记录**，不是每 64 KiB TCP 数据的通用缓存。超过 4096 B 的记录 begin 擦除整区一次，每次 boot recover 另擦整区一次；clear、元数据与清理重试不增擦写。C3／ESP32 的 64 KiB scratch 都不按 64 KiB block 对齐，当前 SDK 成功路径分别使用 16 次 4096 B sector erase。最大记录密文／明文为 65536 B，tag 另 16 B；写入 65536 B，正常完整消费的 17 遍读取加 provider 回验总计 **1179648 B**。来源A的单记录分段及原生调用时长已在本页首段有限观察；完整最坏、独立密码学耗时与OTA并存进展仍未证明，不能把500 ms claim重试策略当作严格最坏墙钟上界。75 个选中输入及 79 个归档成员独立核验；只证明相关 provider／SDK 子集，未宣称全树或全部实际编译路径。

[ESP32-C3 官方数据表 v2.4](https://documentation.espressif.com/ESP32-C3_Datasheet_en.pdf)第 57 页表 5-10 已公布最少 100000 次 P/E 和 20 年数据保持；不能继续称官方耐久数字完全未知，也不能把保持年限等同连续写入寿命。本台 `20/4016` 仍不能唯一定位具体 Flash 型号，[XMC C](https://www.xmcwh.com/uploads/799/XM25QH32C_Ver2.1.pdf)与[D](https://www.xmcwh.com/uploads/920/XM25QH32D_Ver1.3.pdf)共享该 ID；剩余寿命没有实测绑定。

参数化必要关系为 `H + B + 525960 × Y × f + F <= E`：H 为此前每扇区消耗，B 为到达擦除的 boot recover 次数，Y 为使用年数，f 为每分钟成功大控制记录，F 为未计入 f 的失败／取消／重传 begin，E 为适用器件条件下预算；若 f 已含全部 begin，不能重复计 F。维护者的 Y/f 目标仍待答，当前 H／剩余寿命未知，本式不构成寿命通过。官方 19 成员与成本 79 成员均已独立复核，无新持久磨损管理机制。

当前实际新增内存节省仍 **0 B**。CA／config 生命周期复核保留真实 TLS renegotiation 消费，不提前释放仍被引用的配置；原 peer／handshake 和启动栈已释放部分不能重复计省。P6-03、满合法峰值、native 全生命周期、Flash 实板最坏成本、ESP32、断电／72 小时与正式交付继续开放。

## 来源最大事件与完整生命周期续验

2026-10-03，沿用 Base `daf9cd8d46bb22cad7f9cc6656de995a25e4fd89` 的运行源码和上一轮冻结的来源 A／目标 C 签名固件，正式 Wi-Fi、TLS、guest、任务栈、MQTT 与 FRP 预算保持。仓外新入口仅在来源产品 confirmed、网络稳态之后、原一次 OTA 之前增加一个最大事件切片；原 132 项纯测试与新增 12 项均通过，Root 独立复验新增项和入口精确差异。原 OTA ID、90 秒循环及 5 秒只读查询语义、最终绑定与恢复 finally 保持。

实际公开发布帧为 **4096 B**：固定前缀 203 B，来源 `counter v0-1-0` 事件 **3893 B**，含零字节，sequence 2 只发布一次。该 guest 按全部事件字节累加，安装代表事件 19 B 后的真实结果为 **3912**；前后 Wi-Fi connected、MQTT／FRP ready，同设备／boot 和完整 confirmed 绑定保持。目标 `counter v0-2-0` 按 body 字节计数，原目标十二项业务预期不变。这是 OTA 前的单个最大事件，不证明八槽队列、outbox 满载或事件与 OTA 并存峰值。

本轮整轮退出 0。一次 WRITE 联合 OTA 持久确认、USB／认证 MQTT 各一次 stop/start、五个生命周期原 ID 各写一次及重复只读、停止后重启自动运行当前固件 confirmed、旧 stop ID 在新 boot 两通道 unknown、十二项业务、卸载和 A/C 逐字节读回通过。原 OTA ID 查询为 28 次 running、2 次 `ota_result_uncertain`、1 次 succeeded；本轮没有观察到 `storage_uncertain/null`。停止只在当前启动有效，不新增持久停止状态；公开 stop 结果仍不替代独立 native join／句柄仪器。

来源下载 **145/145** 样本 MQTT／FRP ready，连续 prepare 诊断保留 **11 条 NEW_HISTORY、dropped=0**，unsupported／ISR 均 0。历史各 heap minimum 合计 **23228 B < 49152 B**，窗口 current 最低 23888 B；异时历史合计不是同时存活峰值。固定记录区 1800 B、堆起点成本 2832 B 与任务采样工作区 1664 B 均保留，不加回读数，也不与旧正式或其它诊断轮相减归因。451 个完整序列化任务快照的 expected／captured 相等，无采样申请失败；13 个已观察名称的栈余量均至少 1024 B，不证明全部生命周期或满合法峰值。

UART 同一 reader 在明确重启前持续捕获，无重开、错误、溢出或 join 超时；715811 B 日志完整保存，已消费 715769 B、结束待消费 42 B，最高队列量 63815／65536 B，完整诊断 parser 通过，不宣称全部字节均已消费。写前安全状态、两份新鲜一致 4 MiB 基线和三个独立原代码区核对通过；结束擦除全部数据，仅恢复原 bootloader／partition／factory 并逐字节读回，不恢复旧／实验 NVS、不写 eFuse。reset／Wi-Fi down ACK、串口、三 fixture、零所属进程与五个实际精确 listener 释放独立通过。

受限且 Git 忽略的 `c3-validation-20261003/c3-source-maximum-event` 保留原 14142 文件软件冷输入、49 member 新入口 overlay 与 194 member 完整实体证据。overlay 摘要 `9c3bc5d7854595fbd2b3800f73b2c46541ca4d55dd00215912eb7735baf5a1a6`，实体归档摘要 `4be67ca4963561722de9f7d77fdd92ee7e1577c9feb94c2665dadf69d0e7143d`，私有索引 `4cc48fdf8ada092d5fc76c62051588468874c757f4cbc8ff0389ba3fa21cad2f`。Root 再次独立核对 194 个归档成员的字节、大小、mode／执行位与 19 份受限拷贝；旧两轮失败、正式 23888 B 和前轮诊断 22164 B 独立保持。

固定 SDK／实际 ELF 的只读网络生命周期复核确认：未连接的默认 HTTP transport 只是 427 B 请求量，512 B 发送缓冲尚可研究缩短生命周期，两项合计 939 B 请求量不是实测收益；动态 TLS 缓冲已按原生规则释放，peer／handshake 与启动栈不能重复计省。FRP 私有 CA 动态对象量仍待实测，嵌入的常驻证书不能回收。当前实际新增节省为 0。P6-03、满合法队列／在途／并存事件、FRP 双流／最大记录／重连、native 全峰值、Flash 最坏成本、百次公开循环、ESP32、断电／72 小时与正式入口继续开放。

## 连续诊断与公开生命周期复验

2026-10-03，保持上一轮已冻结来源 A／目标 C 的签名固件字节、全部正式资源预算与生产信任不变，仅在仓外驱动的目标确认循环接受两种现有码未决形态：`ota_result_uncertain`／完整原操作元数据，或 `storage_uncertain`／null。后者继续同一原 OTA ID 的只读查询，不产生新 OTA 操作或重发写命令。132 项 pure mocks 通过，Root 独立重跑其中新增 10 项并核对三行精确差异、原恢复代码和全部输入。原单次 5 秒查询与 90 秒循环时序保持，success 先于总 deadline 判定的既有顺序未改，不宣称硬性 90 秒墙钟截断。

新实体整轮退出 0，公开安装／一次 WRITE 联合 OTA 持久成功、USB／认证 MQTT 各一次 stop/start、五个生命周期原 ID 各写一次及重复只读均通过。原 OTA ID 的目标确认回读为 28 次 running、2 次 `ota_result_uncertain`、1 次 succeeded；本轮没有观察到 `storage_uncertain`，该分支由上一轮真实失败和本轮纯测试分别覆盖。停止后重启自动运行当前固件 confirmed，旧 boot 的 stop ID 在 USB 与 MQTT 两通道 unknown；十二项真实业务、卸载、来源 A 保持和目标 C 逐字节读回全部通过，PUBACK 仍只作传输证据。

连续单 reader 从原一次 `ota.start` 前覆盖来源、目标及两种生命周期切片，明确重启请求后关闭；无 backend 重开、捕获错误、队列溢出或 join 超时，768227 B UART 全部记录并消费。64 KiB 有界队列的实际最高待消费量为 64988 B；这只证明本轮范围，不证明更大负载的捕获能力。完整 parser 保留 **10 条 NEW_HISTORY、dropped=0**，unsupported／ISR 均 0，实际 A ELF 返回位置与完整源绑定已核对。来源下载 **146/146** 样本 MQTT／FRP ready，历史各 heap minimum 合计 **22164 B < 49152 B**，窗口 current 最低 26856 B；全部观察成本保留，不与前轮独立读数相减归因或加回。

原始日志另有 477 个完整存活任务快照，expected 与 captured 全部相等，无采样申请失败；13 个已观察任务名的栈余量均至少 1024 B。任务名含平台原样空格，解析保持原值。有限快照不证明全部任务生命周期或满合法峰值；公开 stop 结果与停止窗口不替代独立 native join／句柄仪器。

写前重新读取安全状态、两份完整 4 MiB 基线和三个独立原代码区；初次完整实验 Flash、post-OTA A/C 与结束原三码逐字节核验通过。结束擦除全部实验数据，只恢复原 bootloader／partition／factory，不恢复旧／实验 NVS，不写 eFuse。reset／Wi-Fi down ACK、串口、三个 fixture、零所属进程及五个实测精确 listener 释放独立通过；FRPS remote port 在活动期间由所属进程实际 LISTEN 快照绑定。首次清理复核与两个 Root 只读复核并发，所属进程门禁拒绝；等其结束后原门禁单独回读通过，未减弱门禁或清理未知进程。

受限且 Git 忽略的 `c3-validation-20261003/c3-heap-history-confirmed-observer` 保留原 14142 文件软件冷输入、46 member 新 driver overlay、184 member 本轮完整实体及独立证明。driver overlay 归档摘要 `18c127b4b86771f63c8f7beae6ed6e22f8f2754604ae9aab69eddee8df96401b`；实体归档摘要 `a856d4af618dfe3d1bfff0c63bad4b79206c7086aad27e0cda44271e4723f1c9`，私有索引 `3f30f1a187593e306bb0a15a20a55aee6e45820bde16503cad8e2d60098c72eb`。Root 独立复核全部成员与 19 份受限拷贝的字节、大小、mode、执行位；`c3-heap-history-confirmed-analysis` 的任务／冻结复核补充索引为 `e64ce36feae4fa77897dbde44bfb90f430962ae3deea7a4b47f33a8fb3a39154`，原冻结树未修改。两轮失败及正式 23888 B 生命周期读数均保持独立。

只读内存 owner 复核没有发现第二份 64 KiB 或尚可再次回收的 10 KiB 启动栈；已识别 Container／WAMR 常驻上游申请子集至少 72578 B，不是完整总额。现有小型去重方向尚未实施或证明足以填补缺口，实际节省记为 0。满合法 MQTT 队列／在途与业务事件、FRP 双流／最大记录／重连、独立 native 全峰值、Flash 最坏成本、百次公开循环、ESP32、断电／72 小时及正式入口继续开放；本轮功能与诊断通过，P6-03／五能力容量总门未通过。

## 全 prepare 历史新低诊断与查询暂态失败

2026-10-03，在公开 Base `daf9cd8d46bb22cad7f9cc6656de995a25e4fd89`／OTA `bf11916ab904be4ee9bcdfae213c85336363e96a` 的仓外诊断组合上，捕获范围从实际 `eota_prepare` 入口至返回。固定 1800 B 记录区只保留 `NEW_HISTORY`，连续单 reader 在原一次 OTA 写命令前开启、覆盖来源与目标观测；无 reader 重开、队列溢出或后台错误。观察成本不回加，正式 Wi-Fi、TLS、任务栈与 guest 预算保持。

原始 UART 完整解析出 SUMMARY 与全部 12 条历史触发，dropped、unsupported 和 ISR 均为 0。来源下载 148/148 个 `0 < received < total` 样本 MQTT／FRP ready，历史 minimum 的各 heap 合计为 **22016 B < 49152 B**，窗口 current 最低为 24652 B。两者语义和时点不同；前者不能解释为所有 heap 的同时 live 峰值。实际 A ELF 将首先观察新低的外层 allocator 返回位置映射到 `esf_buf_alloc_dynamic`、`esp_mbedtls_add_tx_buffer`、cJSON 与 RSA 验证；SDK 解锁至 hook 之间允许抢占，PC 不是唯一致因证明，也不能区分不同 TLS 实例。

本轮整体验证失败。目标已启动 pending 产品，一次代表事件返回 guest 结果 2；原 ID 的 27 次只读 OTA 结果中，前 26 次 running，最后一次为代码允许的 `unknown/storage_uncertain/result:null`，过窄驱动断言立即退出。失败后的只读 Flash 快照显示 EOTA v3 308 B 收据仍 PREPARED、ECS2 v2 为 sequence 10／HEALTH_VERIFIED、目标 C 已 VALID；NVS、EOTA、ECS2 与 otadata CRC 及原操作／目标包绑定有效，未发现持久损坏。快照取得晚于查询且经历进入 ROM，不能证明当时是哪次短 Flash I/O claim 失败，也不能证明最终成功。

目标最终确认、后继停止／启动／重启、十二项业务、卸载和 post-OTA A/C 镜像读回均未执行，不复用前轮结果补齐。来源 A 只通过本轮已验签新鲜整片镜像及初次 4 MiB 写后精确读回绑定；目标 C 只绑定冻结输入与原一次 OTA 请求。结束完整擦除实验数据，仅恢复本次独立读取的 bootloader／partition／factory，双新鲜全片基线、三代码区逐字节恢复、reset／Wi-Fi down ACK、串口／三个 fixture／所属进程及四个精确已声明 listener 释放独立通过；旧／实验 NVS 未恢复，无 eFuse 写入。

受限且 Git 忽略的 `c3-validation-20261003/c3-heap-history-partial` 保存完整软件与失败实体证据。源／软件证据归档摘要为 `d2ebb20156cbc4732274f7335323315d4d6800467c6f867ef0ec2824d3e08177`／`f85325e20ac626d57963fb6dee3b5247e8101a81f877ddfd051c70e730918437`，14142 文件全部逐项核验；实体 157 member 归档为 `ad35148cbd3dcf08e05b39c44b36f1687932688ea61cee6e3fe95740842ee161`，私有索引为 `413ecb835748776ec524dc6ec0f77f0d989ec87d0c9b3c50a36ed7f4b333ae49`，Root 独立复核全部字节、大小、mode 与执行位。前轮缺失 SUMMARY 的失败保持独立。

后继仅修仓外驱动：在原 90 秒截止时间内，对现码定义的合法暂态持续查询同一原 ID，不重发写命令、不扩期限、不放宽最终 succeeded 与目标绑定门。只读资源审计尚无实测节省；两个小型 owner 复用方向的未实现上界合计仅 1807 B，不能关闭当前缺口。满合法队列、FRP 双流及最大记录、独立 native 峰值、Flash 成本、百次公开循环、ESP32、断电／72 小时和正式入口继续开放；P6-03 与五能力总门未通过。

## 2026-10-03 新低水探针：部分记录失败

本轮实际运行源码为公开 Base `daf9cd8d46bb22cad7f9cc6656de995a25e4fd89`，精确消费 MQTT `6443b71db761f4d667503f14108687bad5e6b5ee` 与 OTA `bf11916ab904be4ee9bcdfae213c85336363e96a`；仅在仓外叠加既有实验 CA／授权、任务观测及私有分配探针。SDK／lwIP／provider、原生锁、正式 Wi-Fi 32／32、TLS 16384／4096、guest 64 KiB 及原任务栈预算保持。A／C 都为 1183744 B，分别为 `0.2.0-c3-heap-daf9-a/c`，完整构建和官方 RSA v2 验签通过；实际源 A／目标 C 摘要分别为 `76ea92a85851a9e4d535bd517b13efc2ecbbd8cb9b6066bfb753eb1659244090`／`f26c6896cbc5783271ae6b46c8cfe2a27d33164ca0d9954af2b00af02731abe5`。14289 个冻结文件及两份软件归档逐项字节、大小、模式／执行位核对通过。

固定观察区 1800 B、实际 IRAM 增量 1332 B，对应普通堆起点后移 2832 B，均计入现场成本。探针在真实 `eota_prepare` 前准入，正文窗口为 `[0, 327680)`；分配 hook 在 native heap 解锁后读取计数，保留前 11 条与最新末条，largest 在任务边界延后采样。本次实际 C3 ELF 的 RAM／Retention／RTC 类型均同时含 `MALLOC_CAP_DEFAULT` 与 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`，两选择器匹配同一注册 heap 集合；这不扩大到其它目标或配置，也不能将各 heap 异时历史最低值与延后 largest 当作同时存活证明。

公开安装、一次 WRITE OTA、USB／认证 MQTT 停止启动、停止后重启自动运行 confirmed、旧 stop ID 在新 boot unknown、十二项业务和卸载通过；五个生命周期原 ID 各写一次。来源下载 **139/139** 状态 MQTT／FRP ready，采样历史 heap **21360 B < 49152 B**。最后一个旧低水样本为 received 340928 B／25408 B，首次采到最终新低水为 349120 B／21360 B；这一区间已经超出固定探针窗。它是独立诊断 boot 的读数，不能与正式轮 23888 B 相减归因，也不加回观察成本。完整容量仍未通过。

原始 UART 只保存了 index 8 的两个 heap 尾行，以及 index 9／10／11 的三个完整记录，SUMMARY 和前段记录缺失；实际 dropped／unsupported 数无法核定。完整 parser 正确拒绝 dump，`diagnostic_trigger_evidence_available=false`、整轮 `passed=false`，进程退出 1。三个完整尾记录均只有新 window-current 触发，没有 `NEW_HISTORY`；源 A 同一 SHA 的 ELF 只将其返回地址分别映射到 `http_header_init`、`esp_http_client_init`、`esf_buf_alloc_dynamic`。这不能证明历史低水的致因、完整分配时间线或 native 资源回收，不补造 SUMMARY。已确认原观察循环在正常状态查询之间关闭串口，形成无读者区间；没有发现显式 `tcflush`，不把特定 OS／设备缓冲丢弃机制写成已证实事实。

任何解析错误均只聚合为失败，原恢复 finally 继续。Root 独立核对两份新鲜 4 MiB 基线、三个原代码的独立读取／切片／身份 SHA，以及实验 A/C 精确读回；结束整片擦除旧／实验数据，仅恢复原三代码区并逐字节读回，reset／Wi-Fi down ACK、三 fixture 关闭、串口释放、实验所属进程为零和四个精确 listener 不存在均通过。真实恢复没有失败，唯一聚合错误为 `heap_trigger_uart_parse: ValueError`；不写 eFuse、不恢复旧／实验 NVS。

受限且 Git 忽略的 `c3-validation-20261003/c3-heap-trigger-partial` 保存全部失败输入和原始记录。软件源／证据归档 SHA-256 为 `8e450859589de8e8881cb67d3e0942f113d62095defc4be2b241d8bfb8df1bfa`／`ee8a6aee6907f620f44fac8e67110270b9acdf64d05e38fd05962f361689424f`；实体归档为 `29993f8c845062cb6dffba7d5bb322d0451beb2b95ec42f9ca21b300daddbe97`，181 个 member 全部逐项复核。私有索引为 `90c8aa0166942e9acf2f65b99345fc2e7fd23f964c041448918d043ca82fc797`。下一候选需覆盖整个实际 prepare、仅以 `NEW_HISTORY` 占用记录槽、保持连续单 reader，并重新冻结实际 A/C 字节；旧失败保持。ESP32、满合法峰值、Flash 成本、百次公开循环、掉电／72 小时和正式入口继续开放。

2026-10-02，在已发布 Base `ebf2da71449d59365554e12a4efc3fe41e9b117f` 的运行源码上完成三轮私有诊断。三轮 C3 联合功能与原代码恢复均通过；诊断定位了指定窗口内的较大 TLS 分配，没有证明全部瞬时峰值或关闭 48 KiB 容量缺口。

## 输入与诊断边界

固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 和 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，精确 MQTT `f32335852d6f823c1a3b130bfbe3a7ac4499e10a`、FRP `989cc876d92b815aeb0b6806fb861f0ee2b39a86` 及原生双目标锁保持。实际编译的 MQTT 32 份、FRP 40 份源／头文件／构建与 SDK 检查输入逐项核对。源码只在仓外增加诊断和既有实验 CA、timer 授权／一个定时器配置；Main 不含探针，SDK 源码未修改。目标 C 沿用最新精确基线已验签的既有实验镜像，保留其原收据。

三轮 C3 app 均为 1183744 B，官方 RSA v2 验签通过且落在正式 `0x130000` 应用槽内；正式分区、64 KiB guest、TLS IN 16384／OUT 4096、MQTT 队列／outbox、FRP 记录上限和生产授权保持。诊断轮未验证 ESP32 实板。

| 诊断输入 | 记录区／B | 记录结果 | 来源 OTA 历史 heap／B |
| --- | ---: | --- | ---: |
| 原生 HEAP_TRACE_LEAKS，128 条、调用栈深度 0 | 3072 | 首下载回调完整快照 41 条、高水位 54、未溢出；只剩一笔至少 1 KiB 的活跃记录 | 20368 |
| 原生 HEAP_TRACE_ALL，256 条、调用栈深度 0 | 6144 | 周期快照 256 条且溢出；较早分配可被淘汰，不能据此推断全部早期分配 | 18476 |
| 原分配器透传，限定 OTA 任务、至少 1 KiB、128 条 | 2560 | 10 笔完整较大 calloc 记录，无丢弃，实际 ELF 映射调用位置，10 笔释放均有记录 | 23092 |

SDK 的 C3 原生调用栈追踪在没有 frame pointer 时只允许深度 0。首个深度 4 输入被原生配置约束拒绝；随后启用原生 frame pointer 的诊断构建达到 `0x131000`，超过正式槽 4096 B，未刷板。最终使用深度 0。第三轮只在私有组件链接层记录 allocator 调用的返回地址，实际反汇编确认继续调用原 `esp_mbedtls_mem_calloc`／`esp_mbedtls_mem_free`；没有修改 SDK、开启 frame pointer 或改变分配结果。一次选错依赖比对收据形态的校验失败也在刷板前停止，原输入与纠正后的收据分别保留。

## 定向记录证明了什么

第三轮从 `eota_prepare` 入口记录到首个下载进度回调，并继续记录这些地址的释放直到准备返回。只观察同一 OTA 任务中至少 1024 B 的指定 calloc；其它任务、其它分配入口、小分配及 worker 创建不在该统计中。

| 实际 SDK 调用位置 | 捕获大小／B |
| --- | --- |
| `ssl_handshake_init`，由原生动态 `__wrap_mbedtls_ssl_setup` 调用 | 2448 |
| `esp_mbedtls_add_tx_buffer` | 4770、4437 |
| `esp_mbedtls_add_rx_buffer` | 1215、4461 |

128 条容量下仅记录 10 笔且 dropped 为 0，周期串口输出覆盖全部索引 0–9；每笔大小、地址、调用位置与分配后 free 读数在重复报告中一致，随后均观察到 freed。记录中的瞬间 free 最低为 37560 B；这些时点不能代替历史低水、整个握手、其它线程或全瞬时峰值。较大 TLS 分配已释放，不足以解释全部堆低水，不能据此认定 TLS、Wi-Fi 或其它模块是唯一原因。

原生 ALL 轮保留下来的较大记录包括 1024、1532、1700 B，但没有调用位置且已发生淘汰，不能直接将后两种大小全部归为 Wi-Fi。原生 LEAKS／ALL 的准备返回完整快照未被串口日志捕获；只记录已实际收到的完整快照和原始不完整行，不补写不存在的阶段证据。

## 网络分配调用位置续验

同日独立网络探针的 231 份私有证据逐项大小与 SHA-256 核对通过，冻结索引为 `a9856435c47441583d4e97d0c6146f96d9d8be385c96ff71b634d74a2d40d428`。窗口仍从 `eota_prepare` 入口到首个正文进度，只统计非 ISR、cache 开启时 1532／1700 B 的原生 malloc／calloc。原日志捕获四个完整保留摘要、八个组记录：Wi-Fi 的 `esf_buf_alloc_dynamic` 为 1700 B／14 次，tcpip 的 `mem_malloc` 为 1532 B／5 次。Wi-Fi 只映射到函数，源码行不可用；lwIP 为固定 SDK `mem.c:209`。这些是调用次数，不记录释放或同时存活，不证明缓冲数量和优化收益。首进度摘要截断、准备返回完整快照未捕获，保持原证据边界。

实际 ELF 和 52 条保留指令独立核对；两个 wrapper、原分配器及 cache 检查均在 IRAM，cache 关闭时直接透传返回，Flash 记录器仅在 cache 开启后进入。记录区为 1024 B，heap trace／frame pointer 关闭，TLS 16384／4096 B 不变。本轮历史最低 heap 为 24956 B，139 份来源下载状态均 MQTT／FRP ready；正常固件 23800 B 保持独立读数，不能加回记录区字节。公开安装、一次 WRITE OTA、一次 MQTT restart、十二项业务、卸载、全量实验 Flash 与 A／C 镜像回读、实验数据清除和三个原代码区逐字节恢复均核验；四服务和串口释放，无 eFuse 写入。该切片仍未通过 48 KiB 门。

## 联合功能与恢复

每轮都独立核对本次唯一 C3、USB 身份、Flash／安全状态及原三个代码区，完整擦除数据后写入并全量回读实验 Flash。公开安装、一次 WRITE 联合 OTA、一次 MQTT restart、十二项消息计数业务、公开卸载、来源 A 保持及目标 C 镜像回读通过。三轮来源下载分别为 127／130／124 份采样，全部 MQTT／FRP ready；目标和第三 boot 保持新设备身份、revision 3、确认产品及原 OTA 成功。写命令各提交一次。

结束均完整擦除实验数据，只恢复原 bootloader、partition table 和 factory app，三个代码区逐字节核对，原应用 Wi-Fi down ACK、实验 FRPS／网关／HTTPS／MQTT 停止和串口释放均确认。未恢复旧／实验 NVS，未写 eFuse 或生产服务，未操作已拔掉的 ESP32。

## 原生 Wi-Fi 动态缓冲候选

2026-10-02 在 Base `63ea379bb8dbb0f2ddbee44f525a4a45a9b51d1b` 的同一运行源码上，仓外候选仅将 C3 原生动态 RX 从 32 改为 6、动态 TX 从 32 改为 8；静态 RX／BA 窗口仍为 6。A／C 实际编译配置除这两项及官方版本字段外，与前轮网络输入的 active 配置一致，SDK 未改；TLS、guest、MQTT 队列／outbox 与 FRP 上限保持。仍含既有私有内存采样、实验 CA 与 timer 授权，没有 allocator 记录区。此候选未进入正式默认配置。

独立 verifier 的 584 项检查通过，核对源码归档、44 份 SDK 制品、实际配置／ELF、A／C 官方 RSA v2 验签、完整 Flash 与恢复字节。两个镜像均为 1183744 B，落在正式应用槽内。公开一次安装、一次 WRITE 联合 OTA、一次 MQTT restart、十二项消息计数业务及卸载通过，133／133 份来源下载采样均 MQTT／FRP ready。来源下载历史最低 heap 为 **25180 B**，低于 49152 B；同一采样窗口 free／最大连续块／control 栈最低为 37212／27648／2376 B。control 栈为 8192 B，此读数不覆盖所有任务栈或满合法重叠峰值。

本轮写前取得两份相同的完整 4 MiB 新鲜基线；实验镜像全量回读、来源 A 保持和目标 C 精确回读通过。结束整片擦除实验数据，只恢复原三个代码区并逐字节比较，原应用 Wi-Fi down ACK、四服务和串口释放已核对；旧／实验 NVS 未恢复，无 eFuse 写入。此前两次 esptool 输出形态解析失败均发生在 Flash 写入前，原输入和失败日志分别保留。

307 份私有证据保存在 `wifi-dynamic-buffer-candidate`，冻结索引 SHA-256 为 `61d5d190128e9e7174d18ecd8834c057c3d7cd5345c1686f0cb96943e0219612`；独立证明 SHA-256 为 `7f73d15e14542b483e80784d22d0fe91534f0b7348d98dc267a14890170c6ec2`。正常基线 23800 B 是独立样本，不能把差额全归因于 Wi-Fi 修改，也不能据单次功能通过宣布收益或容量通过，因此正式默认值保持。满负荷、完整峰值、Flash 成本、两板、掉电和长稳仍未验收。

## 冻结诊断证据与开放项

ESP Tool 受限、Git 忽略的 `c3-validation-20261002` 独立保存下列输入、原始日志、ELF／map、官方验签、全量回读和清理记录；全部文件摘要核验，旧索引保持。

| 私有目录 | 文件数 | 冻结索引 SHA-256 |
| --- | ---: | --- |
| `ota-native-heap-live-trace` | 205 | `c3c7782ab8c760681424a7e3f7d6e04271f3ab079b2f6d140ba49887e4687d8d` |
| `ota-native-all-allocation-trace` | 193 | `77e1d2599a839fbc9f05f82ce12992b9fabe32b30b9ccb53a0e55dd184f18da6` |
| `ota-tls-allocation-callers` | 206 | `40bf3f71d8d9b683e1b60890dde9418de1e61dec7198ec0e3f65ee88644c4601` |
| `network-allocation-callers` | 231 | `a9856435c47441583d4e97d0c6146f96d9d8be385c96ff71b634d74a2d40d428` |

- [ ] 正常固件仍沿用 [RTC 所有权检查点](rtc-config-ownership-checkpoint.md)的独立读数 23800 B，低于 49152 B 门；不向诊断读数加回记录区字节来标记通过，也不将独立样本差额归给单项修改。其它网络分配来源、满队列／outbox、FRP 双流／预备流／64 KiB 记录、最大输入、全部任务栈和合法重叠峰值、百次整机生命周期、Flash 最坏成本、掉电、72 小时、ESP32 和生产入口继续开放。P6-03 与五能力总门未通过。
