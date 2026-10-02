# C3 新 MQTT 与任务栈诊断检查点

2026-10-02，仓外诊断以 Base `6882effd90aea8ba6358ddff67fadabb28f56550` 的完整源码导出，加公开 MQTT `6443b71db761f4d667503f14108687bad5e6b5ee` 的 manifest／官方生成双目标锁运行。其运行输入对应此前 Base `e302e217f617e30749f286992c2b9115d91f6207` 的软件消费；本轮验证原安装／联合 OTA／重启／卸载，公开停止／启动的实体链另待验收。

正式 Wi-Fi 静态 RX／BA 为 6，动态 RX／TX 为 32／32。TLS 16 KiB／4 KiB、64 KiB guest、协议／队列上限、正式分区、固定 SDK 和 provider pin 保持。私有构建复用既有实验 CA／能力 4／单定时器授权，新增原生 FreeRTOS trace、每秒存活任务快照及 Base worker／guest 线程／main 返回前水位记录。main 只有观测，不调用实验业务；SDK 源码保持。

## 软件与实体链

C3 A／C 两份完整 RSA v2 签名镜像各 1,183,744 B，独立官方验签与原 `0x130000` 槽容量通过。46 项 SDK 制品归档摘要 `a71c3ee0f3a35e8a32041610df932c2f68d96490c1db9351925eca44ba61dd74`；源码归档摘要 `1972541c7b362a74a4b908321eee3d57b8cf881fa597015cc301da7239e44d2e`。两镜像实际 provider 编译单元均匹配公开源码：MQTT 7、FRP 15、OTA 4、Container 10。lwIP 的 771 份 tracked 字节／执行位和两镜像各 80 个实际单元匹配固定 `2758df4cd3666b3b2a5b53830148379326425c0d`。SDK 父 `578cf89c343e388db43ba1f4ddcd602fedcb763c` 只有既有独立 lwIP gitlink 差异，父仓不宣称 clean。

唯一 C3 的身份／安全状态、两个新鲜完整 4 MiB 基线与原三份代码绑定核对后，一轮完成整片 A 写入／字节回读、USB Wi-Fi、严格 TLS MQTT／HMAC 正反例、FRP 认证／错误 key、一次公开产品安装、一次 WRITE 联合 OTA、代表事件及原健康窗、原 ID 持久成功、一次 MQTT restart、同设备第三 boot／revision 3／已确认产品恢复、十二项业务正反例、卸载和 A／C ROM 全字节回读。142／142 份来源下载状态均为 MQTT／FRP ready。

结束后擦除实验数据，仅恢复原 bootloader／partition／factory 并逐字节核对；原 Wi-Fi-down ACK、四项服务与控制 loop 停止、串口释放均通过。未写 eFuse，未保留旧或实验 NVS，未访问 ESP32、mac-ci-2 或生产。

## 实际观测

325 份完整原生存活任务快照均 expected／captured／记录行数相等，无丢行、申请失败或容量溢出；另有 8 份返回前记录。采样工作区每次申请 1664 B，复制 TCB 名称时保持调度器暂停，随后只打印自有名称并清零释放。原生 trace 的 TCB 开销及工作区开销均计入堆观测，没有回加。记录水位单位由固定 SDK `StackType_t` 字节合同验证。

| 任务 | 本轮已记录最低余栈 B |
| --- | ---: |
| base_control | 2392 |
| IDLE | 1196 |
| tcpip | 2576 |
| wifi | 4512 |
| esp_timer | 3648 |
| Tmr Svc | 1724 |
| sys_evt | 2048 |
| mqtt_task | 2688 |
| esp_frp | 3264 |
| base_product | 6084 |
| pthread | 4556 |
| main | 2572 |
| base_ota | 6576 |

来源 OTA 串口段有 22 份内存采样：历史最低堆 22,040 B、本次 free 最低 35,044 B、采样最大连续块最低 28,672 B、控制任务最低余栈 2392 B。142 份协议状态的来源下载历史低水同为 22,040 B，低于 49,152 B。目标 C 与第三 boot 的历史低水分别 41,076／36,736 B，独立分段记录，不与来源合并推断收益。

这些记录覆盖实际采到的任务及本轮业务，不能证明全部任务完整生命周期、最大 guest 运行路径或所有合法叠加峰值。返回标记发生在其 printf 前，仍有后续少量返回路径。采样连续块／当前任务水位不能代替全门；P6-03 及五能力总门继续失败。与先前正常镜像 23,800 B 或 Wi-Fi 候选 25,180 B 的差值不用于计算收益或回补观测开销。

## 冻结与修正

220 项完整私有证据在 ESP Tool Git 忽略受限路径 `c3-validation-20261002/mqtt-native-task-snapshot`，索引摘要 `ad4db59ceca41398f062f8f403aef41ebf1768edd20e39eb745ba327a726b039`。独立核验 600 项全部通过，回执摘要 `c2f904f6eeda94703ee8057e630e71c69a77d9a4256d75271060186c5881800b`；任务分析摘要 `9286bc7150899a64fd1c7f9c487f79f81a57cbe834ebea55a5181033fa7ec631`。

首轮编译因私有 Container 观测 header 的组件 include 边界失败；保留源／日志，改为同仓明确相对 include 后在新目录重建，未新增循环组件依赖。分析器初版只接受无空格任务名，遗漏 SDK 原样 `Tmr Svc`；修正解析后同一原始串口逐份核对 325 个快照，无固件或设备改动。观察 helper、原生 trace、实验配置与所有私有输入均保持仓外，正式 Wi-Fi 默认值不变。

满规格三条 MQTT 排队与第四条在途、FRP 双活跃加预备流及最大合法记录、完整 OTA／无线重连峰值、最坏 Flash 成本、公开停止／启动实体链、ESP32、真实断电、72 小时与生产验收继续开放。
