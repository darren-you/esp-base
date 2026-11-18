# ESP Base 业务应用 SDK（Rust）

把设备业务写成 Rust 应用、编译成 WASM，由 [esp-base 固件](../../README.md)里的 WASM 宿主加载运行。应用与固件分开构建：改业务不需要重新编译固件。

- `esp-base-app`：SDK。`no_std`，不依赖 `core::fmt`，出错直接陷阱。
- `examples/hello`：最小样例。每秒计数一次，业务 MQTT 连上后发布计数，收到消息原样回显。
- `fixtures`：测试用的应用：故意触发各种故障的，把宿主接口逐个走一遍的（`io`），在设备上操作引脚的（`pins`）。

## 快速开始

需要 [rustup](https://rustup.rs)。工具链版本和编译目标 `wasm32v1-none` 由 `rust-toolchain.toml` 固定，第一次构建时自动安装。在本目录执行：

```bash
cargo build --release -p hello
ls -l target/wasm32v1-none/release/hello.wasm   # 约 1.5 KB
```

把应用封装成可以部署的[应用包](../../docs/design/app-package.md)（在仓根执行，签名需要已导出的官方 ESP-IDF 环境）：

```bash
python3 tools/esp_base.py app build sdk/rust/examples/hello
python3 tools/esp_base.py app sign sdk/rust/target/esp-base/hello.ebapp --key-file /absolute/path/signing-key.pem
```

## 写一个应用

应用是事件驱动的：宿主调用一次 `init`，之后每个事件调用一次 `event`，同一时刻只有一次调用在执行，所以应用里没有线程，也不需要锁。

```rust
#![no_std]
use esp_base_app::{log, mqtt, timer, App, Error, Event};

struct Blink {
    ticks: u32,
}

impl App for Blink {
    fn init() -> Result<Self, Error> {
        log::info("blink: init");
        mqtt::connect(None, 30)?;
        timer::set(0, 1000, 1000)?;
        Ok(Blink { ticks: 0 })
    }

    fn event(&mut self, event: Event) {
        if let Event::Timer(0) = event {
            self.ticks += 1;
            let _ = mqtt::publish("blink/tick", b"1", mqtt::Qos::AtMostOnce, false);
        }
    }
}

esp_base_app::app!(Blink);
```

应用能用的设备能力只有 SDK 里的这些：

| 模块 | 能力 |
| --- | --- |
| `log` | 日志，出现在设备的串口日志里 |
| `uptime_ms`、`unix_ms`、`sleep_ms` | 单调时间、UTC 时间（没对时为 `None`）、短暂睡眠 |
| `timer` | 4 个定时器，到期时应用收到 `Event::Timer` |
| `mqtt` | 业务 MQTT：连接与遗嘱、订阅、发布，消息以 `Event::MqttMessage` 送达 |
| `param` | 业务参数：出厂资料里 `business` 对象的字符串值，按键读取 |
| `device` | 设备 UUID、芯片型号 |
| `gpio` | 数字引脚：读、写 |
| `pwm` | PWM 输出：频率、分辨率、占空比 |

业务 MQTT 的地址和账户由系统保存，应用拿不到；连接、重连和重新订阅都由宿主完成。

## 引脚

引脚要先在 `Cargo.toml` 里声明用途，没有声明的引脚应用碰不到（`Error::Denied`）：

```toml
[package.metadata.esp-base.pins]
4 = "out0"       # 输出，安全电平低
5 = "out1"       # 输出，安全电平高
6 = "in_pullup"  # 输入：in、in_pullup、in_pulldown
7 = "pwm"        # PWM 输出，没在输出时为低
```

```rust
gpio::write(4, true)?;                 // out0、out1 能写，也能读回实际电平
let pressed = !gpio::read(6)?;         // 输入脚只能读
pwm::config(7, 50, 14)?;               // 50 Hz、14 位：舵机
pwm::write(7, 1228)?;                  // 占空比 0 到 2^14-1
```

输出脚后面的数字是安全电平：设备启动后、应用的代码运行之前，应用停止、出故障、被替换之后，宿主都把引脚保持在这个电平，`pwm` 引脚保持低电平。应用不用自己在 `init` 里「先把输出关掉」，也不用担心出故障时输出停在半途。每种芯片能用哪些引脚见[应用包](../../docs/design/app-package.md#引脚)。

## 约束

- **体积就是内存。** ESP32-C3 上应用整体载入内存，每大 1 KB 就多占 1 KB。不要用 `format!`、`core::fmt` 和带格式的 `panic!`；数字转字符串自己写（见样例）。
- **没有堆。** 状态放在应用结构体里，缓冲用定长数组。
- **每次调用有指令预算。** 死循环或算得太久的调用会被宿主停掉，应用随后被重启。长时间的事拆成定时器事件。
- **睡眠是短的。** 应用睡着时事件都在排队，所以一次调用（`init` 或一个事件）里累计最多睡 1000 毫秒，超出的那次返回 `Error::TooLarge`、不睡。换向前停 200 毫秒这样的事可以睡，更长的等待用定时器。
- **出错即陷阱。** `panic`、越界、把线性内存之外的指针交给宿主接口，都会让应用停止并重启；系统层不受影响。
- **清单写在 `Cargo.toml` 里。** `[package.metadata.esp-base]` 的 `memory` 是应用的内存上限（运行时内存池，字节），`stack` 是操作数栈；可选的 `chips` 限定能装的芯片，`[package.metadata.esp-base.pins]` 声明引脚。
- **链接参数不能改。** `.cargo/config.toml` 把栈设为 4 KiB 并导出 `__heap_base`、`__data_end`，宿主据此把线性内存收缩到实际用量；没有收缩的应用会被拒绝。

## 测试夹具

改了 SDK、样例或 `fixtures` 之后重新生成宿主测试用的应用，并把产物一起提交：

```bash
./build_fixtures.sh                                # 写到 ../../firmware/tests/apps/
bash ../../firmware/tests/run_wasm_host_tests.sh   # 用这些应用跑宿主测试
```
