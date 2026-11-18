# ESP Base 业务应用 SDK（Rust）

把设备业务写成 Rust 应用、编译成 WASM，由 [esp-base 固件](../../README.md)里的 WASM 宿主加载运行。应用与固件分开构建：改业务不需要重新编译固件。

- `esp-base-app`：SDK。`no_std`，不依赖 `core::fmt`，出错直接陷阱。
- `examples/hello`：最小样例。每秒计数一次，业务 MQTT 连上后发布计数，收到消息原样回显。
- `fixtures`：宿主测试用的应用，每个故意触发一种故障。

## 快速开始

需要 [rustup](https://rustup.rs)。工具链版本和编译目标 `wasm32v1-none` 由 `rust-toolchain.toml` 固定，第一次构建时自动安装。在本目录执行：

```bash
cargo build --release -p hello
ls -l target/wasm32v1-none/release/hello.wasm   # 约 1.5 KB
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

应用能用的设备能力只有 SDK 里的这些：日志、时间、定时器、睡眠和业务 MQTT。业务 MQTT 的地址和账户由系统保存，应用拿不到；连接、重连和重新订阅都由宿主完成。

## 约束

- **体积就是内存。** ESP32-C3 上应用整体载入内存，每大 1 KB 就多占 1 KB。不要用 `format!`、`core::fmt` 和带格式的 `panic!`；数字转字符串自己写（见样例）。
- **没有堆。** 状态放在应用结构体里，缓冲用定长数组。
- **每次调用有指令预算。** 死循环或算得太久的调用会被宿主停掉，应用随后被重启。长时间的事拆成定时器事件。
- **出错即陷阱。** `panic`、越界、把线性内存之外的指针交给宿主接口，都会让应用停止并重启；系统层不受影响。
- **链接参数不能改。** `.cargo/config.toml` 把栈设为 4 KiB 并导出 `__heap_base`、`__data_end`，宿主据此把线性内存收缩到实际用量；没有收缩的应用会被拒绝。

## 测试夹具

改了 SDK、样例或 `fixtures` 之后重新生成宿主测试用的应用，并把产物一起提交：

```bash
./build_fixtures.sh                                # 写到 ../../firmware/tests/apps/
bash ../../firmware/tests/run_wasm_host_tests.sh   # 用这些应用跑宿主测试
```
