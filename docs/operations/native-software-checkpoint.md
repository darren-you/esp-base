# ESP Base 软件验证入口

本页维护可复现的软件验证入口。每轮源码、制品、失败日志、测试数量和实板资格见 [Issue #35](https://github.com/darren-you/darren-space/issues/35)；历史冻结记录仍由 Git 与仓外证据归档保留，不将不同轮次拼为一次通过。

## 固件与工具

- [固件说明](../../firmware/README.md)定义双目标装配、依赖锁、签名与分区；[设备协议](../design/device-protocol.md)定义 USB、MQTT、认证分块和原 ID 结果。
- [测试说明](../../firmware/tests/README.md)提供 host、控制 owner、分块与存储故障测试入口。宿主假件不证明实体 Flash 时延、断电原子性或功耗。
- [宿主工具](../../tools/README.md)提供固定 SDK 构建、完整制品校验、官方分区/NVS/签名预检及容量解析入口。旧布局只读校验保留原设备身份、revision、配置与恢复材料。
- [首次有线布局迁入](../../firmware/components/ota_operation/README.md#首次有线布局迁入)使用双份完整新鲜恢复原件，普通 app OTA 不改分区、bootloader 或 eFuse。

## 固定输入与证据

双目标独立冻结完整 sdkconfig、公开验签键、SDK 派生、组件锁、工具链和 Core 原始版本快照。正式构建从已保存源码和固定依赖全新解析，测试键产物明确标为 software-test，不能进入生产设备或正式制品登记。

设备成功须原 operation_id、实际新 boot、完整 signed bin 摘要/长度/target/槽及独立固件观察共同匹配；传输结束、PUBACK 或单次重启均不够。软件回归、正式服务发布、本机 App、完整 SDK 和实体 R1—R7 的资格分别记录，具体门见[验收合同](ota-allocation-diagnostic-checkpoint.md)。
