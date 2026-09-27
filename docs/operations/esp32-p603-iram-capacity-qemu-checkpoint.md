# P6-03：现有 ESP32 的 IRAM 容量隔离实验

2026-09-28。维护者确认继续使用现有 ESP32-C3 和 ESP32-D0WD-V3 两块 4 MiB 板，48 KiB 内部 8BIT 堆历史最低空闲门不变。本实验延续[FRPS／TLS 诊断](esp32-frps-phase-union-current-lock-qemu-checkpoint.md)的 Base `1fe24302f5ec94dfc30180eb86369dbdc62774b8`、FRP `0af12209ee731617e635684309c026ae6b49c5ae`、固定 ESP-IDF `578cf89c`／lwIP `2758df4`、64 KiB ABI 2 Wasm guest、签名 4 MiB Flash、OpenETH、本机官方 FRPS v0.71.0、测试 CA／IP SAN 和额外 4 KiB 探针任务。原动态 TLS 缓冲诊断的签名输入在认证阶段内存不足，最低空闲 1,524 B；本轮只检验 ESP32 可字节访问 IRAM 的分配方向，未修改正式产品配置、FRP 源仓或实体设备。

两档都使用 SDK 原生 `CONFIG_FREERTOS_UNICORE=y`、`CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY=y`、`CONFIG_MBEDTLS_IRAM_8BIT_MEM_ALLOC=y` 与已有 `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`，保留 `SSL_IN_CONTENT_LEN=16384`、`SSL_OUT_CONTENT_LEN=4096`、FRP 的 `MBEDTLS_SSL_VERIFY_REQUIRED`、证书日期和主机名校验。固定 SDK 的该分配策略只将达到 TLS 输入／输出缓冲阈值的分配优先交给 IRAM；普通分配仍使用内部 DRAM。SDK 明确禁止把此 IRAM 用作 FreeRTOS 任务栈，单核运行也是该特性的前置条件。第二档在仓外受管 FRP 副本将 `efrp_session_t` 的单次零初始化分配**严格**移至 `MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT`，没有回退到普通堆，并为探针增加 IRAM 剩余量；FRP 仓正式源码未变。源业务包、64 KiB 标准 Wasm 页及 Container 配额未缩小。

| 证据 | SDK IRAM TLS 大缓冲 | 再将 FRP 会话本体移入 IRAM |
| --- | ---: | ---: |
| 仓外目录，位于 `mac-work-1:/private/tmp/` | `esp-base-p603-iram-tls-20260928` | `esp-base-p603-iram-session-20260928` |
| 签名 app SHA-256／长度 | `62adb07ef4d8aa3f93e87ab0bed585cc6fc53c1f26fd5b56a4013a2783e00623`／`0x10fff4` | `380bd6e11872eed2538997cef5649fd14235648a5594d041433e2bc1d6b77691`／`0x10fff4` |
| 种子 Flash SHA-256 | `a5bc5a31c91f87ba75af95e805c67e4b79d732d339a53f2a1f5815f142716607` | `61168fd2f263fb47283dd75ad644e57f7aaabd9d6b4f5b4ea0ecc1fd7e7ced86` |
| QEMU 结果 | 登录、代理注册、Pong；`ready=1` | 登录、代理注册、Pong；`ready=1` |
| FRP 就绪时普通 8BIT 堆空闲／最大块 | 25,076／23,552 B | 39,928／38,912 B |
| 本次启动普通 8BIT 堆历史最低空闲 | **17,248 B** | **32,100 B** |
| 会话就绪时 IRAM 空闲／最大块 | 未加探针 | 68,308／49,152 B |
| 原始 UART SHA-256 | `8b5047b1752738e8998100695e26ab40741088395a309ae0c1143f7a4f17ab91` | `6eb23d5b6ada045bed0ee346e30eb2c48df5f62e6e2d50af593be56313a7bcd7` |

两档均由官方 ECDSA v1 签名工具验证 app 与分区表，官方容量门确认 `0x120000` 双 app 槽仍可装入；再按各自 app 完整 SHA 重新生成 ECS2 和全新种子 Flash。QEMU 启动再次验证签名，宿主 FRPS 与 QEMU 均在脚本退出时清理。第二档从 `before_create` 到 `AUTHENTICATING` 的 IRAM 空闲由 83,160 降至 68,308 B，和普通堆历史最低值增加的 14,852 B 一致。两档没有可见分配失败；FRP client 就绪快照的 `verify=UINT_MAX` 是尚未复制 TLS 状态的初始值，不能读作证书验证结果。实际 TLS 源码在握手结束后读取 `mbedtls_ssl_get_verify_result()` 并在非零时拒绝进入 `OPEN`，FRP 登录和 Pong 因而证明了该路径已通过测试 CA 的严格验证。

第二档仍比 **49,152 B** 门少 **17,052 B**，还没有 MQTT、OTA、正式 Base FRP owner 或实体 Wi-Fi 同存；此时 Base 的正式 SNTP 仍未就绪，固定测试时钟仅服务仓外探针。ESP32-C3 的 SDK RAM 布局没有同等的独立 `MALLOC_CAP_IRAM_8BIT` 区域，本方案不能直接外推到 C3。单核调度、IRAM 字节访问性能、真实 Flash／网络和长稳未测，仓外受管组件改动也不属于源仓实现。P6-03 与 P7-02 保持未验收；下一个容量切片必须同时验证剩余至少 17,052 B 的真实释放与 C3 的独立预算，不能凭配置、无网络构建或缩小 Wasm 页宣称达标。
