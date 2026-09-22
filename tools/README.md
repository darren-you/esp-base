# USB 主机调用示例

此目录运行在 macOS/Linux 宿主；不属于 MCU 固件，也不依赖私有 ESP Tool。

## 架构拓扑

```mermaid
flowchart LR
    user["开发者：本轮端点与设备 UUID"] --> cli["device-control.py"]
    cli -->|"独占串口 / JSON Lines"| firmware["ESP Base device_protocol"]
    firmware -->|"状态、结果、启动 ID"| cli
```

先退出占用该端点的监控或烧录程序；工具仅使用 Python 3 标准库。从本轮系统枚举结果选择端点，不把历史端点当设备身份。

```bash
python3 tools/device-control.py --port /dev/cu.usbmodemEXAMPLE status
python3 tools/device-control.py --port /dev/cu.usbmodemEXAMPLE --device-id <刚核对的UUID> restart
```

默认输出块状摘要，`--json` 输出设备 JSON。重启先读取状态、精确绑定 UUID/boot/deadline，收到 `running` 后再次查询同设备的新启动，才报告成功；超时为 unknown，写命令不自动重试。直接打开 POSIX 串口，使用 `flock` 和 `TIOCEXCL` 独占当前端点，不切换 DTR/RTS，并关闭 HUPCL；串口写入限一秒，以设备回执确认执行。实板发现串口库逐次清除 DTR/RTS 会触发额外 USB 复位，因此状态读取也必须验证不会改变 boot_id。完整 probe/flash/恢复编排由设备工具负责。

配置使用当前用户拥有、权限 0600 的本机 JSON 文件，不把密码放在命令行或输出中：

```bash
python3 tools/device-control.py --port /dev/cu.usbmodemEXAMPLE --device-id <刚核对的UUID> --config-file <本机私有配置文件> config.set
```

文件包含完整 schema_version、wifi、mqtt、frp、business 字段。当前 schema_version 为 1；wifi 为 ssid/password 对象或 null，其余三项为 null。工具读取新鲜 revision 后构造 CAS 请求，最多等待 30 秒；仅确认新 revision 后报告成功。文件不存在、权限不合格、重复字段或内容无效会拒绝，不回显配置。

`python3 tools/test-device-control.py` 使用本机伪终端验证字节不变、禁用关闭挂断和写入背压期限；伪终端不证明物理 USB 复位行为，后者以同板重复打开后的 boot_id 与断电验收为准。
