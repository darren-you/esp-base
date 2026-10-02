# C3 OTA 分配诊断检查点

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

正常固件仍沿用 [RTC 所有权检查点](rtc_config_ownership_checkpoint.md)的独立读数 23800 B，低于 49152 B 门；不向诊断读数加回记录区字节来标记通过，也不将独立样本差额归给单项修改。其它网络分配来源、满队列／outbox、FRP 双流／预备流／64 KiB 记录、最大输入、全部任务栈和合法重叠峰值、百次整机生命周期、Flash 最坏成本、掉电、72 小时、ESP32 和生产入口继续开放。P6-03 与五能力总门未通过。
