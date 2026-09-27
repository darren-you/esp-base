# P6-03 当前锁命令载荷容量检查点

2026-09-27，以 Base `402836044c714c42891e5439790a07bfd3658738` 建立独立工作树，核对其精确消费的 MQTT `c0677e5e779c3e51e814f2920420be7ec54f1d88`。本轮仅合并 Base 命令结构中互斥的载荷；未修改 MQTT 源仓、分区、签名策略、设备或生产 Broker。C3／ESP32 依赖锁 SHA-256 分别为 `9ee783f487a527a0c050aabce754683bf21f41490b163239d8c42019c293193e`／`5510c046f2fe68bf05e18afa3ff657954160d52f2c2b5874c8efd709e5ba03b8`。

## 生命周期与别名边界

- [`esp_base_protocol.c`](../../firmware/components/device_protocol/esp_base_protocol.c) 的 `s_context.config` 在启动时从 NVS 读取，随后供状态、Wi-Fi 回退、MQTT／FRP 重配读取；`s_candidate` 则在 `config.set` 被接纳后保留完整候选，等待至多 20 秒的连接证明。此时旧配置与候选同时存在；失败时必须用旧配置恢复，不能复用同一存储。`command.config` 是解析及规范指纹的工作值；即使候选期拒绝新写入，仍须先解析、计算指纹、完成去重/期限裁决，因此不能覆盖 `s_candidate`。证明成功后的同步 NVS 提交才借用已结束解析的 `command.config` 作为第三份 `work`；API 要求它与 candidate、committed 均不同，返回前擦除。
- [`esp_base_remote_config.c`](../../firmware/components/remote_config/esp_base_remote_config.c) 的 `s_load_bytes` 与 `s_commit_bytes` 各为最大 v3 blob 的 7,618 B。前者由启动加载和控制任务内同步指纹计算复用，计算后擦除；提交时后者保存待写规范字节，前者在 NVS 读回后保存重新编码的字节，两者必须同时存在以逐字节比较。改成一个缓冲区会丢失独立读回证明；改为控制任务局部数组会超过其 6 KiB 栈容量。
- [`mqtt_owner.c`](../../firmware/components/device_protocol/mqtt_owner.c) 的 `s_work` **已经是**配置／事件 union。配置只在 `configure` 期间使用，锁定 MQTT 的 `emqtt_create` 会复制配置；事件由 `poll` 填充。不能再把 `s_work` 和 Base `command` 合并：MQTT `MESSAGE` 的请求 view 借用 `s_work.event.message.payload`，同步回调 `handle_mqtt_command → handle_line → ebase_parse_command` 才写 `command`；若两者重叠，解析入口的整结构清零会先破坏仍在解析的请求字节。
- [`command_decoder.c`](../../firmware/components/device_protocol/command_decoder.c) 的每次解析先清零完整 `ebase_command_t`。`config.set`、`ota.start`、`ota.result` 走互斥分支，分别只写 `config`、`ota`、`operation_id`；status/restart 无载荷。`request` 和指纹独立于载荷，故规范指纹、去重和回执不因 union 改变。配置一经接纳即复制到 `s_candidate`；OTA 在创建异步 worker 前复制到 `s_ota_request`，失败建任务时仍在本次同步处理内读取 `command.ota.operation_id`；`ota.result` 只在本次同步查询中读取 operation ID。错误路径保留 `request_id` 用于失败回执，擦除最大载荷成员；新增编译期断言保证它覆盖 OTA 和 operation ID 的所有字节。下一命令解析再次清零整个结构。

因此只把 [`esp_base_command.h`](../../firmware/components/device_protocol/include/esp_base_command.h) 中三个互斥载荷放进匿名 union，调用方字段名、JSON、持久 blob、候选与 OTA 异步所有权均不变。上述其余大缓冲区均有真实并存反例；本轮没有转为运行时堆申请。

## 固定 SDK 对照

`mac-work-1` 仓外旧版和新版独立源码/构建副本位于 `/private/tmp/esp-base-dram-buffer-audit-20260927-{baseline,modified}`，原始日志及 map 位于 `/private/tmp/esp-base-dram-buffer-audit-20260927-evidence/`。两份源码的基点相同，仅新版有上述产品代码改动；同一新增定向回归先在旧版运行，再在新版运行：有效 MQTT 配置后解析新 `status`，不得残留密码；部分解析失败须擦除配置载荷且保留 request ID。固定 ESP-IDF 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`，版本检查通过。每份源码执行 `ESP_BASE_TEST_TARGET=esp32c3 bash firmware/tests/run_host_tests.sh` 和 `ESP_BASE_TEST_TARGET=esp32 bash firmware/tests/run_host_tests.sh`，均使用 ASan／UBSan；旧版、新版各为 C3 **20/20**、ESP32 **19/19** 通过，包含 decoder、OTA owner 与配置存储用例。旧版和新版的 host 日志分别同为 SHA-256 `d150db972747e6dffde5f742490b9da3f403215bca62fb6788f3723d315924a1`（C3）及 `e940c700729a1692ae418df8749bc26501dd42c6f862767e7eef20bf328e6e53`（ESP32），因为通过摘要文本相同；测试脚本实际从各自源码重新编译。

两版分别执行 `idf.py -C firmware -B <独立 build> -D SDKCONFIG=<独立 sdkconfig> -D IDF_TARGET=<目标> build`；ESP32 额外按仓内合同加 `-D ESP_BASE_ESP32_OFFLINE_PROBE=ON`，该无签名普通构建仅用于离线检查。四次完整构建均通过。同目标新旧 `sdkconfig` SHA-256 逐字节相同：C3 `962a52515a5b61e2910e02070f88b4f2ab2af3063e974a0dac744e516c06f775`，ESP32 `46305890efd00df2745c6bb1f694a199e338aaf99683adc5ddbdb0a96522ffdd`。

| 目标 | 旧 `.bss.command` | 新 `.bss.command` | 旧 `.dram0.bss` | 新 `.dram0.bss` | 旧 `_heap_start` | 新 `_heap_start` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ESP32-C3 | `0x20d0` | `0x1e58` | `0x15f28` | `0x15cb0` | `0x3fca9610` | `0x3fca9390` |
| ESP32 | `0x20d0` | `0x1e58` | `0x162c8` | `0x16050` | `0x3ffca868` | `0x3ffca5f0` |

两目标的 `command` 和整段 `.dram0.bss` **均精确减少 `0x278`（632 B）**；`s_candidate=0x1db8`、`s_context=0x1dd4`、MQTT `s_work=0x1e24`、两个 NVS 编码缓冲各 `0x1dc2`，前后大小不变。旧／新 C3 map SHA-256 为 `485d8b71402e561d87e6f88cd6bb9ddaeda3a6b19668c2dba8b9d11571abf4fa`／`09dead38664d585824de58ee3cf83b4a58ee149d50846158e4a4bdc3041f30a5`；旧／新 ESP32 map 为 `41dcf6d84677b09272753184d3eced615fe52a6bc2b4193159e11b8b8eb58e3f`／`056c291de98a286de35e4a3e30cbb653a0d78ff9afb6a0f7fd48e8a4a3365aa6`。

这些是普通离线构建的静态链接结果，尚无此源码组合的签名产品镜像、QEMU 或实体板运行堆最低值；不能把 632 B 直接加到早期 guest/FRPS 探针读数后宣称达到 48 KiB 门。P6-03 五能力同存与实板验收仍未完成。
