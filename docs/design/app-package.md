# 应用包

应用包是一个签名的、可以独立部署的业务应用：一个 WASM 模块加一份清单。它与固件分开构建、分开签名、分开下发，固件用同一把键验签。打包工具（`tools/esp_base.py app`）、设备（`firmware/wasm/base_wasm/core/wasm_package.c`）和 Server 都按本文实现；样例 [`firmware/tests/apps/hello.ebapp`](../../firmware/tests/apps/hello.ebapp) 是它的可执行版本，设备的 host 测试直接读它。

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
{"name":"hxh_lift","version":"1.0.0+12","abi":1,"memory":12288,"stack":2048,"chips":["esp32c3"],"pins":{"4":"out0","5":"out0"}}
```

| 键 | 取值 | 含义 |
| --- | --- | --- |
| `name` | 1–23 个小写字母、数字或下划线，字母开头 | 应用名 |
| `version` | `X.Y.Z+构建号`，各段是不带前导零的十进制数 | 应用版本；本地构建的构建号是 0 |
| `abi` | 正整数 | 应用需要的最低宿主接口版本。固件提供的版本低于它时拒绝（`abi_mismatch`） |
| `memory` | 4096–65536 | 运行时内存池的字节数，也就是应用的内存上限：模块、实例和执行环境都在里面。超过芯片允许的上限时拒绝（`memory_too_large`） |
| `stack` | 1024–8192 | 应用的操作数栈，字节 |
| `chips` | 可选；1–4 个芯片型号（状态文档里的 `chip.model`） | 只允许装到这些芯片上；不写表示不限。不在列表里时拒绝（`chip_mismatch`） |
| `pins` | 可选；1–8 项「引脚号: 用途」的对象 | 应用要用的引脚，见下节。不写表示不用引脚。芯片上不能这样用时拒绝（`pin_denied`） |

## 引脚

应用只能碰清单里声明过的引脚，而且只能按声明的用途用；没有声明的引脚，宿主接口一律返回「拒绝」。键是十进制的 GPIO 号（不带前导零），值是用途：

| 用途 | 应用能做的 | 应用没在运行时 |
| --- | --- | --- |
| `in`、`in_pullup`、`in_pulldown` | 读电平 | 保持输入（带声明的上拉或下拉） |
| `out0` | 写电平、读回实际电平、有时限的输出 | 输出低电平 |
| `out1` | 写电平、读回实际电平、有时限的输出 | 输出高电平 |
| `pwm` | 设定频率和分辨率、写占空比、读回占空比 | 输出低电平 |

「应用没在运行时」的状态就是安全状态：设备启动后、联网之前，应用的代码运行之前，应用停止、出故障、被替换或回退之后，以及应用因为连续拖垮设备被停用期间，宿主都把这些引脚保持在安全状态。所以接了执行机构的引脚，要按「断电时该是什么电平」来选 `out0` 还是 `out1`。应用被换成不再声明某个引脚的版本、或设备上不再有应用时，这个引脚回到高阻。

有时限的输出是给执行机构用的：应用把输出脚置到某个电平并给一个时限，到时宿主把它置回安全电平。到期由宿主自己的定时器直接执行，不经过应用——应用的事件会因为一次卡住的网络操作迟到十几秒，「最迟多久必须断开」这样的要求不能靠应用自己的定时器保证。

设备没有应用、或进入安全模式（业务层不运行）时，所有引脚都是芯片复位后的状态：硬件要保证这时也是安全的。

哪些引脚能用由固件按芯片判定，有一个不行整个包就不装：

| 芯片 | 能用的 GPIO | 不能用的 |
| --- | --- | --- |
| ESP32-C3 | 0–10、20、21 | 11–17 接内置 Flash；18、19 是 USB Serial/JTAG |
| ESP32 | 0、2、4、5、12–19、21–23、25–27、32–39 | 6–11 接 Flash；1、3 是下载和日志用的 UART0；34–39 只能作不带上下拉的输入 |

每个 `pwm` 引脚占一个 LEDC 通道：ESP32-C3 最多 6 个，ESP32 最多 8 个。启动时要采样的引脚（ESP32-C3 的 2、8、9，ESP32 的 0、2、5、12、15）可以声明，但外接电路不能在复位时把它们拉到会改变启动方式的电平。

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
python3 tools/esp_base.py app run <.ebapp、.wasm 或工程目录>             # 本机运行：按设备的规则验收清单，不验签
```

新建工程（`app new`）、本机测试（`app test`）和本机运行的用法见 [SDK 说明](../../sdk/rust/README.md)。

`app build` 的清单取自应用工程的 `Cargo.toml`：`name`、`version` 来自 `[package]`（包名里的连字符换成下划线），`memory`、`stack` 和可选的 `chips` 来自 `[package.metadata.esp-base]`，可选的 `pins` 来自 `[package.metadata.esp-base.pins]`，`abi` 按模块实际导入的宿主接口算出，构建号用 `--build-number` 给出。工具只核对 `pins` 的写法；某个引脚在某种芯片上能不能用由设备在安装时判定。

```toml
[package.metadata.esp-base]
memory = 12288
stack = 2048
chips = ["esp32c3"]

[package.metadata.esp-base.pins]
4 = "out0"
5 = "out0"
```

## 正式包

给正式设备用的应用包与固件一样由 Fast Deploy 的 package Job 在构建宿主上构建和签名，签名键不离开那里。Job 由产品仓声明：`platform` 为 `plugin`，`variant` 是应用名，`architectures` 是 `[wasm32]`，入口调用本仓的 `tools/app_package_entry.py`，环境变量 `ESP_BASE_APP_PROJECT` 指向应用工程、`ESP_BASE_APPLICATION_ID` 等于 `package.id`，签名键与信任根的两个变量和固件相同。应用工程 `Cargo.toml` 的 `version` 必须等于 Fast Deploy 分配的版本，依赖按入库的 `Cargo.lock`（`cargo build --locked`），构建目录在本次运行的目录里，不改动源码树。

产出的 ZIP：

| 文件 | 内容 |
| --- | --- |
| `<应用名>.ebapp` | 签名的应用包；登记到 ESP Tool Server、部署到设备的就是它，它的 SHA-256 就是登记和设备上报用的那个 |
| `<应用名>.wasm` | 包里的模块，供排查 |
| `Cargo.lock` | 这次构建用的依赖锁 |
| `manifest.json` | `kind` 为 `esp-base-app`；清单（`app`）、构建输入（Rust 工具链、锁与编译配置的摘要、签名信任根的公钥摘要）、模块体积与线性内存、各文件的字节数和 SHA-256 |
| `build.json` | Fast Deploy 写出的本次身份，逐字节相同 |

入口只构建、签名和封装：登记（`app push`）和部署（`app deploy`，或在 ESP Tool 网页上）是之后分开的两步。
