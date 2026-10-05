# FRP 唯一配置 owner 软件检查点

2026-10-03，从公开 Base `9b1e64ec409676a2dcc06104ec97e9ba7a5b4d81` 的完整导出建立仓外候选。FRP owner 不再常驻复制 2730 B 完整配置；它借用同一 control task 持有的 canonical 配置。本文仅记录软件验证与原生链接收益，不表示 C3 容量或实体交付通过。

## 所有权与失败路径

canonical 位于启动至重启期间存活的 `s_context`；C3 保持既有 RTC 放置，ESP32 保持既有策略。启动读取失败不创建 control owner；启动成功后只有 control task 修改配置。正常提交只在 NVS 写入与完整读回核对成功后发布新 revision。UNCERTAIN 重新读取复用该轮已有 `work`，仅完整读取且 Flash claim 释放成功后一次发布；失败解码即使已部分写入 `work`，也不破坏已准入 canonical。随后立即清零释放 work，再进行 Wi-Fi 和结果发布，没有新增持久 owner。

FRP owner 在 configure 入口先关闭创建门；旧 native handle 的 worker、DNS、回调和 Flash 清理完成后才读取新配置。销毁失败保留旧 handle 和已准入指针状态，每轮沿原 revision 流程重试；poll 在重配置或 draining 未结束时不解引用 canonical。禁用以 NULL 指针表示，在销毁完成后关闭 native 实例。旧 management listener 仍在换版本前撤销，原 TLS、可信时间与本地认证门不变。

精确消费的 FRP `989cc876d92b815aeb0b6806fb861f0ee2b39a86` 在 `efrp_create` 内同步复制 endpoint、CA、token、proxy、设备身份与 Flash store，再启动 native worker；worker 不借 Base canonical。网络恢复和重连在旧对象回收后从已准入 canonical 创建新对象。没有新增兼容副本、第二 owner 或线程。

## 软件验证

两个目标完整 ASan/UBSan host 回归通过，新增覆盖原址换版本、destroy 的 WOULD_BLOCK／存储失败重试、禁用、重连，以及 UNCERTAIN reload 成功／部分输出后失败。独立源码审计核对启动、唯一变更者、listener、native 深拷贝和所有回收分支。第一次 host／SDK 后的 work 释放顺序收敛已在最终源码上重复 host 与四个构建的增量重编译、链接和验签，旧日志保留为过程证据。

两个目标普通和签名固定 SDK 构建完成；C3 官方 RSA v2 与 ESP32 官方 ECDSA v1 验签通过，原应用槽容量保持。原 manifest、两个普通 native lock 及两个 NVS probe lock 不变；正式 OTA Git pin 仍为 `bf11916ab904be4ee9bcdfae213c85336363e96a`，没有 override。

| 原生构建 | 普通 BSS 减少／B | 普通堆起点前移／B |
| --- | ---: | ---: |
| C3 普通 | 2728 | 2720 |
| C3 签名 | 2736 | 2736 |
| ESP32 普通 | 2736 | 2736 |
| ESP32 签名 | 2736 | 2736 |

FRP 的完整配置符号由 2730 B 变为 4 B 指针，表内结果包含实际 linker 对齐；RTC data 范围未变。上述静态收益不能直接加到独立实体历史最低值上宣称容量通过。

实际 SDK 配置均保持动态 TLS buffer、入站 16384 B／出站 4096 B，未启用 DYNAMIC_FREE_CONFIG_DATA，peer certificate 不保留。FRP 的 1036 B pending 明文副本未在本候选改动；它与内部加密 record 的生命周期不同。

## 容量边界

既有诊断来源历史最低 heap 22040 B 距 49152 B 仍差 27112 B。该轮最后一次历史低水发生在 uptime 124033–125035 ms；同窗的 control、OTA、MQTT、FRP 和 guest native stack 额定分配共 49152 B，guest 的固定线性页 65536 B 与显式 Wasm stack 8192 B 同存，已知请求下界为 122880 B，尚未计 TCB、VM 元数据、系统任务或网络瞬时分配。这些额定量用于定位同存基础，不是 allocator 归因。

本候选未缩小任何任务栈、guest、合法并发、4 KiB MQTT、64 KiB 控制记录、TLS 上限或正式 Wi-Fi 动态 RX/TX 32/32；保留安全验签及原槽。未运行设备、网络实验、mac-ci-2、发布或凭据变更。后续实体必须在同一正式镜像核验该静态收益的实际 heap、原合法峰值与失败清理；完整容量门仍未通过。
