# 应用包

应用包是一个签名的、可以独立部署的业务应用：一个 WASM 模块加一份清单。它与固件分开构建、分开签名、分开下发，固件用同一把键验签。打包工具（`tools/esp_base.py app`）、设备（`firmware/components/base_wasm/core/wasm_package.c`）和 Server 都按本文实现；样例 [`firmware/tests/apps/hello.ebapp`](../../firmware/tests/apps/hello.ebapp) 是它的可执行版本，设备的 host 测试直接读它。

## 布局

全部小端。

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | 魔数 `EBAP` |
| 4 | 2 | 格式版本，目前是 1 |
| 6 | 2 | 包头长度，固定 32 |
| 8 | 4 | 清单长度，1–1024 |
| 12 | 4 | wasm 长度 |
| 16 | 16 | 保留，全 0 |
| 32 | | 清单（UTF-8 JSON），紧跟 wasm 模块 |
| | | 用 `0xFF` 补齐到 4 KiB 边界 |
| 4 KiB 的整数倍 | 4096 | 官方 Secure Boot V2 签名块，覆盖它之前的全部内容 |

- 包的身份是整个文件（含签名扇区）的 SHA-256。
- 整个文件不超过 32 KiB（一个应用槽），所以包头、清单和 wasm 合计不超过 28 KiB。
- 签名与固件相同：同一把 RSA-3072 键、同一个 `espsecure sign-data --version 2`。设备用官方 `esp_secure_boot_verify_signature` 验签，信任根是当前运行固件签名块里的公钥，所以应用只能装到用同一把键签名的固件上。魔数把应用包和固件镜像区分开，两者不会被互相当成对方。

## 清单

一个 JSON 对象。键是固定的，多余、重复或缺少的键都会让整个包被拒绝。

```json
{"name":"hello","version":"0.1.0+7","abi":1,"memory":8192,"stack":2048,"chips":["esp32c3","esp32"]}
```

| 键 | 取值 | 含义 |
| --- | --- | --- |
| `name` | 1–23 个小写字母、数字或下划线，字母开头 | 应用名 |
| `version` | `X.Y.Z+构建号`，各段是不带前导零的十进制数 | 应用版本；本地构建的构建号是 0 |
| `abi` | 正整数 | 应用需要的最低宿主接口版本。固件提供的版本低于它时拒绝（`abi_mismatch`） |
| `memory` | 4096–65536 | 运行时内存池的字节数，也就是应用的内存上限：模块、实例和执行环境都在里面。超过芯片允许的上限时拒绝（`memory_too_large`） |
| `stack` | 1024–8192 | 应用的操作数栈，字节 |
| `chips` | 可选；1–4 个芯片型号（状态文档里的 `chip.model`） | 只允许装到这些芯片上；不写表示不限。不在列表里时拒绝（`chip_mismatch`） |

## 对 wasm 模块的要求

打包时检查，设备加载时再核对一次：

- WebAssembly 1.0 指令集（Rust 的 `wasm32v1-none` 目标）。
- 导出函数 `app_init` 和 `app_event`。
- 只从模块 `base` 导入函数，而且都是宿主接口里有的。
- 没有 start 段：实例化时执行的代码不受指令预算约束。
- 没有 `memory.grow`，并导出全局变量 `__heap_base` 和 `__data_end`：宿主据此把线性内存收缩到实际用量（栈加静态数据），上限 16 KiB。

## 工具

在仓根执行，需要 rustup 和已导出的官方 ESP-IDF 环境：

```bash
python3 tools/esp_base.py app build sdk/rust/examples/hello            # 构建、检查并封装，产出未签名的 .ebapp
python3 tools/esp_base.py app sign <.ebapp> --key-file /absolute/path/signing-key.pem
python3 tools/esp_base.py app inspect <.ebapp 或 .wasm>                 # 清单、体积、用到的宿主接口、线性内存
```

`app build` 的清单取自应用工程的 `Cargo.toml`：`name`、`version` 来自 `[package]`（包名里的连字符换成下划线），`memory`、`stack` 和可选的 `chips` 来自 `[package.metadata.esp-base]`，`abi` 按模块实际导入的宿主接口算出，构建号用 `--build-number` 给出。
