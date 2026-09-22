# 开发检查点

2026-09-22 P2 配置/Wi-Fi 检查点：在同一 C3 上完成 config.set、独立 base_store 分区的完整配置条件提交、候选验证、Wi-Fi RAM 配置与退避重连。当前开发镜像为 760576 字节，SHA-256 `5097fb549ff792680a311144b44340f5470a78bcd11b3694d29e77d07d684773`。

- 错误 SSID、错误口令均未增加 revision，原已提交配置重新连接成功；正确配置、重复请求原结果、同 ID 不同内容冲突、旧 revision 拒绝、显式清除与重新配置均通过。
- 软件重启后同 UUID、新 boot、原 revision 和 Wi-Fi 连接恢复通过。Wi-Fi 在线时 100 次 USB 查询无重启，free heap 范围 211180–211216 字节；这不是五能力峰值或长稳测量。
- Bridge HTTP 配置成功校验新 revision，20 秒失败候选返回明确失败并保留原配置；公开 CLI config.set 通过。最终 revision 为 5。
- 两次完整 Flash 读回摘要一致，核对身份 UUID、NVS header/data CRC、blob 当前索引和完整配置字节；revision 与 Wi-Fi 凭据同 blob 保存，启动区、分区、OTA selector 未改。最终恢复件仅保存在 ESP Tool 私有忽略目录。
- 人工拔除 USB 电源后重新接通，确认真实 power_on、新 boot、同 UUID、revision 5 与 Wi-Fi 自动恢复。首次尝试暴露主机串口库打开端口会额外复位，修复为 POSIX 直接打开并禁用关闭挂断后复测通过；公开 CLI 连续打开三次保持同 boot，伪终端字节传输与背压期限测试通过。
- ASan/UBSan 覆盖命令解析、配置编解码、revision 冲突/耗尽和 SDK 存储调用的故障注入；ESP-IDF 编译通过。上述断电发生在配置已提交后，不证明 Flash 写入中间态掉电；候选/提交期间断电、WPA3 和物理插拔完整矩阵尚未验收，P2 保持实施中。

以下 USB 检查点对应本轮较早镜像，保留其范围明确的验收事实。

2026-09-22 P2 USB 检查点：持久身份、每次启动 UUID、严格 JSON Lines 解析、命令 guard、状态结果及重启回执已接入真实 USB。ESP-IDF v6.1 / ESP32-C3 编译与 ASan/UBSan host 回归通过；cJSON 固定为官方 `1.7.19~2`，提交依赖锁。

- 在迁移基线对应的同一块 4 MiB C3 上重新核对 USB、NVS 身份和有效 OTA 选择，取得两份一致的完整 Flash 恢复件；只写入既有 `ota_0` 应用。
- 验证错误设备、旧 boot、过期、超长期限、未知命令、错误参数、重复字段、错误类型、非法 UTF-8、8193 字节超限、分片和半帧超时恢复。连续 100 次状态查询没有重启，free heap 始终为 322036 字节。
- 实板超长输入暴露官方缓冲驱动 RX ring 满后丢数据的问题，改用官方无缓冲 VFS 和 FIFO 背压后整组通过。公开 Python 示例的 status/restart、私有 Bridge 的 HTTP→USB 状态/重启/重复请求/旧 boot 拒绝均通过。
- 写后读回确认启动区、分区表、身份 NVS、OTA selector 和 `base_store` 逐字节未变，应用读回摘要一致；没有修改 eFuse、分区布局或 GPIO。

本轮应用为 160800 字节，SHA-256 `9bfbb1ab10b6dbd0dabed4286c40ebcf075d79bc65422b63ae7409b43d982456`。真实身份、原始日志和恢复字节只进入 ESP Tool 私有忽略目录，不进入公开仓。上述短时 USB 测试不证明配置/Wi-Fi、MQTT、FRP、OTA 或 72 小时长稳；P2 尚未完成。

以下为 2026-09-21 历史基线，当时尚未刷写实板。

2026-09-21 开发基线：ESP-IDF v6.1 / ESP32-C3 构建通过；命令身份、boot、期限、去重与容量 host 回归通过 ASan/UBSan。当前设备协议仍是迁入心跳；命令 guard 尚未接入实际执行，配置/Wi-Fi/MQTT/FRP/OTA 继续实施。未刷写实板。

开发基线已保存至 `master` 的 `16a903e15d2fea2fe5c09a9aa17f622c5821da21`。已从 GitHub 重新 clone 到工作区之外的全新目录进行独立验证，不复用原 checkout 的源码或构建产物。 ESP-IDF v6.1 / ESP32-C3 编译及 ASan/UBSan 命令裁决测试通过；应用大小 0x25120（151840）字节，单槽 0x1e0000，剩余约 92%。

正式发布与完整硬件验收仍未完成。旧独立 checkout 结论只覆盖上述历史 commit；本轮 USB 改动尚未提交或推送。
