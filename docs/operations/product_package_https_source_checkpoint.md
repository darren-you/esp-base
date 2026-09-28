# 产品包 HTTPS 来源检查点

2026-09-29，Base 为 P6-04 的公开安装／升级事务加入产品包顺序读取器。调用方必须先核对本次启动可信时间、授权设备与持久操作意图；读取器只接受有明确路径的 HTTPS URL，不接受 URL 凭据、片段、反斜杠、控制字符、空格或无效端口。它通过 ESP-IDF 证书 bundle 验证 TLS，关闭自动重定向，只接受 HTTP 200、非 chunked、与已授权包长逐字节相同的 Content-Length。Container 槽写入回调只能按连续 offset 顺序读取；短读、重复／跳跃 offset、迟到字节、未完整接收的响应均失败，不能把未完成的网络正文交给候选 Flash。

连接／单次读／无进展／总期限分别为 5 秒／1 秒／30 秒／5 分钟。每次 SDK 调用返回前后核对单调时钟，迟到的响应不能续期。固定 SDK 的 DNS、TLS 或 HTTP 单次调用仍不可抢占，因此这些数值尚不是严格墙钟返回上界；实际设备 DNS、证书链、慢滴流和长期 Flash 传输仍需 P6-04/P7 测量。

当前只提供可由 Container 候选槽 `source_fn` 直接消费的有界来源原语，尚未接公开 `product.install`／`product.upgrade`、账本登记、候选写入 worker 或试运行确认。生产 app 未调用该原语，普通签名镜像的容量数字不能计作安装链路的完整链接成本；这一步不改变设备包或分区。

## 已验证

- C3 与 ESP32 全套 host ASan／UBSan 入口通过。新增故障假件核对证书 bundle、禁止重定向、URL／长度／状态拒绝、连续分片、EAGAIN、无进展到期和正文未完整时拒绝。假件不执行真实 TLS。
- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 的仓外 C3 `0x77000` 候选正式 USB 测试键签名构建通过，新增来源 C 文件进入 IDF 组件归档；镜像仍为 `0x121000` B、每个 app 槽余 `0xf000` B。由于生产入口尚未引用该符号，链接器未将其计入镜像，不能据此证明新增功能适配该容量。
- ESP32 同源固定 SDK 显式离线构建通过；该 unsigned probe 不可刷写。

宿主与构建日志保留在 `mac-work-1:/private/tmp/esp-base-package-prepare-20260928/`。下一步需在 Base 产品 worker 中先持久登记原 ID 与精确请求指纹，再用此来源把候选送入 Container；网络或槽状态不确定时保留 claim，试运行最终健康判据与真实 MQTT 业务事件另行闭合。

2026-09-29 续验：响应头与正文读取遇到立即返回的 `ESP_ERR_HTTP_EAGAIN` 时，来源线程现在各让出一个 FreeRTOS tick，再重新核对无进展及总期限；避免单核 C3 在持续无数据期间紧循环占用 Wi-Fi 和控制任务。宿主假件分别让两个阶段连续返回 `EAGAIN`，均在 30 秒模拟无进展期限后拒绝并清理；双目标共享的全套 host ASan／UBSan 回归通过。同一源码另按固定 SDK 现存 C3 RISC-V 与 ESP32 Xtensa 的 `compile_commands.json` 原参数分别执行真实编译器语法检查，双目标均通过。生产入口仍未链接，设备网络响应和调度延迟仍待后续验证。
