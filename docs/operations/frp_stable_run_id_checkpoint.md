# FRP 稳定 run ID 接线检查点

2026-10-02，Base 使用已有设备 UUID 作为 FRP `client_id` 与请求 `run_id`，消费公开 esp-frp `fcbce1cd2a7a740c1fe04db9dc7d3dd7df66d5f7` 的 0.2.0 API。该值是请求身份；只有完成严格 TLS、Token 登录、代理注册和认证 Pong 后才能报告 ready。没有新增 NVS、身份生成或凭据更换。

该接线解决的触发条件是：设备重启后，官方 FRPS 仍保留上个 boot 的控制连接。服务端新生成的 run ID 会与同 client_id 的旧控制连接冲突；稳定请求身份允许新连接在完整鉴权后接管旧控制连接。[组件检查点](https://github.com/esp-space/esp-frp/blob/fcbce1cd2a7a740c1fe04db9dc7d3dd7df66d5f7/docs/operations/stable_run_id_checkpoint.md)记录旧连接存活、错误 Token 拒绝且保留旧 READY、正确 Token 冷实例替换，以及请求值与已鉴权状态的隔离。

## 软件验证

Base owner 的 ASan/UBSan 回归核对请求 run ID 等于既有 client_id。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与其精确 lwIP 组合分别完成 C3／ESP32 构建、测试键签名、官方验签及完整 host 回归。没有刷写 ESP32 或改动生产授权。

| target | 签名 app 字节 | app 槽字节 | 签名 |
| --- | ---: | ---: | --- |
| esp32c3 | 1183744 | 1245184 | RSA／Secure Boot V2 格式，测试键 |
| esp32 | 1114100 | 1179648 | ECDSA／Secure Boot V1 格式，测试键 |

普通两份依赖锁由固定 SDK 的 Component Manager 生成，FRP 精确提交与内容摘要均更新；两份不消费 FRP 的 NVS 探针锁逐字节保持。没有手工填写组件摘要。

首次普通构建虽然清单已更改，Git 依赖仍沿用旧锁提交，因新字段缺失而失败。普通 C3 使用 `idf.py update-dependencies` 后重新解析；ESP32 使用独立 `dependencies.lock.esp32`，需以 SDK `ComponentManager(..., lock_path='dependencies.lock.esp32').update_dependencies()` 指定该锁，再由实际 target 的 `idf.py reconfigure` 生成。固定 SDK 中未带显式路径的更新动作只处理默认锁，不能据此认为两目标都已更新。失败日志与正式重编译收据均保留。独立 FRP 样例源码根必须保留 `esp-frp` basename，仓外目录名引起的首轮失败也独立保留。

## 实板与容量边界

C3 联合 WRITE OTA／MQTT 重启后的 FRP 恢复将使用新签名实验镜像复测。先前[五能力失败](../issues/c3_product_frp_mqtt_ota_capacity.md)的 109 份证据和数据丢弃／原代码恢复结果保持不变，不用软件测试改写实板结论。

身份修正不代表容量优化。此前来源下载最低普通 heap 为 6500 B，48 KiB 门未通过；最大记录、MQTT 满队列、连续块、任务栈、两板、掉电、72 小时及生产入口继续开放。
