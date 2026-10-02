# C3 五能力稳定身份联合检查点

2026-10-02，公开 Base `ffc88efbbb259b32ca75c944776ba77328f731b6` 与精确 FRP `fcbce1cd2a7a740c1fe04db9dc7d3dd7df66d5f7` 在 C3 完成代表业务的联合功能切片。此前第三 boot 的 FRP 登录冲突已修复；内存容量门仍失败，不能据此宣布五能力验收完成。

## 实际执行与结果

设备从空实验布局启动，使用原始应用入口、正式分区和精确依赖锁。仓外配置仅注入实验 HTTPS CA、timer 能力与一个定时器，目标镜像版本为 `0.2.0-c3-lab-run-id-c`；没有改动生产授权或注入测试调用入口。两份 app 均为 1183744 字节，固定 SDK 测试键 RSA V2 签名及官方验签通过。

公开配置建立 Wi-Fi、严格 TLS MQTT 和官方 FRPS 0.71.0。Tool 原有控制库验证实际设备 PSA HMAC 的 FRP 状态；错误管理 key 被空 401 拒绝，正确 key 仍可读取。来源 counter 经实际 HTTPS 下载、验包、代表事件和原连续 30 秒健康门确认；产品、MQTT 与 FRP ready 时再执行一次 WRITE 联合 OTA。

88 份来源下载采样均为 MQTT／FRP ready。目标新 boot 的消息计数包经代表事件和原健康门确认，原 OTA ID 持久 succeeded，并通过 FRP 认证状态读取。随后只发送一次 MQTT restart，第三 boot 的 USB／MQTT 状态与 FRP 认证状态确认设备身份、revision 3、已确认产品和原 OTA 成功结果保持。目标与第三 boot 的 FRP 观察均首次采样即 ready，不再出现之前 90 秒的 LOGIN_REJECTED。

十二项实际消息计数、暂停恢复、定时窗口到期和业务负例检查通过；业务负例正确返回 business_failed／负结果与发布器退出码 2。公开卸载完成。安装、联合 OTA、MQTT restart、卸载各执行一次，没有重发写命令。原 OTA ID 的 32 次只读观察为 28 次 running、3 次带有绑定结果的 unknown、1 次 succeeded。

完整初始 Flash 回读、来源 A 保持和目标 C 签名镜像全字节回读通过。实验结束后 ESP 上的实验数据丢弃，原三份代码制品逐字节恢复；原代码的 Wi-Fi down ACK、全部实验服务停止和串口释放均核对完成，没有 eFuse 写入。ESP32 未连接，未执行该板实测。

## 容量仍未通过

| 阶段 | 最低本次 free heap／B | 最低历史 heap／B |
| --- | ---: | ---: |
| 来源产品的 FRP 认证状态 | 47696 | 16980 |
| 来源下载的 88 份采样 | 9080 | 4124 |
| 目标 C 的 FRP 认证状态 | 46940 | 23892 |
| MQTT restart 后第三 boot 的 FRP 认证状态 | 47584 | 22876 |

既有状态接口的 heap 观测均低于普通内部 8BIT 堆 49152 字节门；身份修正不作为内存优化。最大连续块、任务栈、MQTT 满队列／在途消息、FRP 双流／预备流与满长记录、全部合法重叠峰值仍需分项测量和修正。不能用本切片的业务成功替代容量、两板、掉电、72 小时或生产入口验收。

## 冻结证据

本轮 143 份私有实板证据索引 SHA-256 为 `effd1620aa917549bd660eb246e00b050f48e80fc0747a630be06b80f2da2988`。软件与 SDK 的 78 份独立索引为 `6afed73993a23f226ddfc47b8fbb9eb81b7bfe3297a2c1a3a13fb7afe7ccb624`，其中 194 份编译输入与公开 Base 的运行文件逐字节核对，四份锁均验证。私有证据目录与索引已确认被 Git 排除，文件 0600／目录 0700；设备 UUID、MAC、凭据、签名私钥与恢复字节不进入公开仓。

此前 109 份[失败证据](../issues/c3_product_frp_mqtt_ota_capacity.md)保持不变。[软件接线检查点](frp_stable_run_id_checkpoint.md)记录 API、鉴权替换和双目标 SDK 边界。
