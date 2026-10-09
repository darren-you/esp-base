# ESP Base 原生应用

`main/esp_base_main.c` 装配持久设备身份、配置、Wi-Fi／时间、严格 TLS MQTT、设备 FRP、原生消息计数与签名固件 A/B OTA。不配置 GPIO 输出。C3 使用 USB Serial/JTAG VFS，ESP32使用UART0；构建、目标、SDK、双锁和分区入口见[固件说明](../../README.md)。

控制 owner 同步执行认证原生业务，保留状态、暂停／恢复、100ms非延期窗口和重启初始化。MQTT reported区分认证接受与业务结果；USB、MQTT和FRP共用严格设备／boot／原ID裁决。完整字段与有界输入见[设备协议](../../../docs/design/device-protocol.md)。

固件升级采用182B V4收据和精确来源／候选签名身份。写前意图与读回成功后启动唯一OTA worker，FRP有界上传和USB触发的HTTPS来源复用同一写槽／验签机制。pending新boot依本地初始化和控制进展连续30秒确认，不要求Broker／FRPS在线；失败回滚，不确定保留原operation及claim，最终持久读回成功才报告succeeded。旧V3／损坏／未决记录阻断普通升级。详细恢复及一次性旧布局解析见[OTA操作](../../components/ota_operation/README.md)。

启动先核对真实槽与分区，再读取身份和配置；FRP scratch恢复失败时停止本次初始化，不能确认pending槽。C3／ESP32分别使用`frp_scratch@0x3e5000`／`frp_scratch@0x3ea000`，均为`0x10000`。网络等待不持有短Flash claim，真实配置提交、OTA读写／槽观察和FRP scratch操作复用同一存储owner。Wi-Fi失败如实报告，USB控制不因网络失败而虚报成功。

配置使用既有EBCF v3单blob，凭据由本轮真实配置消费，不生成或更换有效密钥。SNTP沿`CONFIG_ESP_BASE_TIME_SERVER`启动；当前boot可信时间和Wi-Fi IP是URL型OTA来源的前置，普通未签名构建拒绝升级。首次分区改变由受控有线迁入完成，应用OTA不改布局或eFuse。

软件输入、宿主假件与实际结果见[原生软件检查点](../../../docs/operations/native_software_checkpoint.md)。本轮双板实体、容量、断电、百次／连续72小时以及正式交付仍按[唯一执行计划](../../../docs/operations/ota-allocation-diagnostic-checkpoint.md)取得资格；旧实验记录不构成当前通过。
