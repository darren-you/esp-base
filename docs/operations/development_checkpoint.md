# 开发检查点

2026-09-21 开发基线：ESP-IDF v6.1 / ESP32-C3 构建通过；命令身份、boot、期限、去重与容量 host 回归通过 ASan/UBSan。当前设备协议仍是迁入心跳；命令 guard 尚未接入实际执行，配置/Wi-Fi/MQTT/FRP/OTA 继续实施。未刷写实板。

开发基线已保存至 `master` 的 `16a903e15d2fea2fe5c09a9aa17f622c5821da21`。已从 GitHub 重新 clone 到工作区之外的全新目录进行独立验证，不复用原 checkout 的源码或构建产物。 ESP-IDF v6.1 / ESP32-C3 编译及 ASan/UBSan 命令裁决测试通过；应用大小 0x25120（151840）字节，单槽 0x1e0000，剩余约 92%。

正式发布与硬件验收仍未完成。构建与 host 测试不替代实板。
