# P6-03 命令去重表常驻容量检查点

2026-09-27，从 Base `6ef7a028c2a5e16ea62fab6a0942bf7500479cd9` 建独立分支，只精简设备命令去重表的历史槽。固定 SDK 为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；C3／ESP32 组件锁文件 SHA-256 分别为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`、`5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`。仓外固定 SDK 构建与日志保存在 `mac-work-1:/private/tmp/esp-base-p603-guard-compact-20260927/`；没有访问实体设备、凭据、正式分区或生产制品。

## 判定不变

解析及 wire 请求仍是完整的 `ebase_request_t`。`ebase_admit` 每次先验证本次请求的三个 UUID，并分别比对当前 device ID 与 boot ID，随后才查看历史 request ID、到期时刻和完整 32 字节指纹。原实现写入历史槽的 device ID 与 boot ID 此后没有读取者；同 request ID 的异步结果仅从历史槽读取 request ID。主固件的静态 guard 由唯一控制任务消费，设备 ID 指向启动时读取的静态 identity，boot ID 只在 `protocol_start` 生成且成功启动后不再重建；即使单独调用 guard API，旧判定也不读取历史两项身份。因此历史 entry 仅保存 request ID、原期限与指纹，保留 32 槽、先验身份拒绝、重放、冲突、容量和异步回执语义。

`command_guard_test` 新增已接纳同 request ID 后，调用方当前 device ID 或 boot ID 改变时仍先拒绝；原有过期后同 ID 重放、期限／指纹冲突、满槽及无驱逐用例继续通过。Base 完整 host ASan/UBSan 回归在 C3 **20/20**、ESP32 **19/19** 通过；原始日志 SHA-256 为 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`、`e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`，包含真实协议异步结果路径与命令解析测试。

## 固定 SDK 链接结果

相同 SDK／依赖锁的旧无采样普通构建与本分支独立普通构建直接比较；ESP32 编译显式启用仅供离线检查的 `ESP_BASE_ESP32_OFFLINE_PROBE=ON`。C3、ESP32 构建均通过。两目标链接图的 `s_guard` 都从 `0x1308`（4,872 B）降到 `0x0a08`（2,568 B），**各释放 `0x900`（2,304 B）常驻 `.bss`**：

| 目标 | 旧 `.dram0.bss` | 新 `.dram0.bss` | 旧 `_heap_start` | 新 `_heap_start` |
| --- | ---: | ---: | ---: | ---: |
| C3 | `0x16828` | `0x15f28` | `0x3fca9d90` | `0x3fca9490` |
| ESP32 | `0x16bc8` | `0x162c8` | `0x3ffcb028` | `0x3ffca728` |

旧 C3／ESP32 map SHA-256：`d4d75478325a3ee4e5c3091d4135df779a77de098f1f037088b735b24af173d5`、`48182b67fa50827cc0a661f559263d586cba2497c4344b6c16401659dc82ff8e`；新 map：`689828a85de6357aff295b7c11c38981183fea8b862ff60dfb62a376d7f5b44d`、`be06d0d5df48ad82a0289aac9e44d02f323ad8bb121269c9ef397c09b62910a2`。新普通镜像分别为 `0xdefe0`／`0xd2bf0` B，SHA-256 为 `8332620f8eddd43dc0d2528b2eb297168437ec44a4113b6ba34bd5911a3d56c7`、`c4a6f520173eeadf8628434886973a045f73262100d82389477af7f849c5b6e1`；构建日志 SHA-256 为 `7b7c655ecaad3ae77e73f96a17531c80234cdc4c154c5a7f685047849f76780e`、`45b0d4e2c74d1d7c699969489d26c244fca00b44480495b4b9ee29a37b35ecbe`。

本次只证明静态 DRAM 腾出 2,304 B，**没有重测签名 guest 的运行堆最低值**；不能把旧 QEMU 的 49,100 B 直接加上该数后宣称跨过 48 KiB 门。FRPS/TLS、MQTT、HTTPS OTA 与 guest 同时运行和实板验收仍未完成，P6-03 保持未通过。
