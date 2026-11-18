# ESP Base 业务应用 SDK（Rust）

把设备业务写成 Rust 应用、编译成 WASM，由 [esp-base 固件](../../README.md)里的 WASM 宿主加载运行。应用与固件分开构建：改业务不需要重新编译固件。

- `esp-base-app`：SDK。`no_std`，不依赖 `core::fmt`，出错直接陷阱。
- `examples/hello`：最小样例。每秒计数一次，业务 MQTT 连上后发布计数，收到消息原样回显。
- `fixtures`：测试用的应用：故意触发各种故障的，把宿主接口逐个走一遍的（`io`），在设备上操作引脚的（`pins`）。

## 快速开始

需要 [rustup](https://rustup.rs)。工具链版本和编译目标 `wasm32v1-none` 由 `rust-toolchain.toml` 固定，第一次构建时自动安装。下面的 `esp_base.py` 是本仓的 `tools/esp_base.py`，应用工程可以放在任何地方（别的仓库里也行）：

```bash
python3 tools/esp_base.py app new ../my-app        # 新建工程：一个能跑的最小应用，约 0.4 KB
python3 tools/esp_base.py app test ../my-app       # 本机跑单元测试
python3 tools/esp_base.py app run ../my-app        # 本机运行：模拟定时器、业务 MQTT 和引脚
python3 tools/esp_base.py app build ../my-app      # 构建、检查并封装成未签名的应用包（.ebapp）
python3 tools/esp_base.py app sign ../my-app/target/esp-base/my_app.ebapp --key-file /absolute/path/signing-key.pem
```

`app run` 和 `app sign` 用到固件那一侧的东西：前者要 cmake、C 编译器，以及组件管理器给固件取回的 WAMR 和 cJSON（先在已导出 ESP-IDF 环境的终端里构建过一次固件）；后者要官方的 `espsecure`。[应用包](../../docs/design/app-package.md)的格式、清单和引脚规则见设计文档。

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
| `gpio` | 数字引脚：读、写、有时限的输出 |
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
gpio::pulse(4, true, 2000)?;           // 有时限的输出：2 秒后宿主把它置回安全电平
let pressed = !gpio::read(6)?;         // 输入脚只能读
pwm::config(7, 50, 14)?;               // 50 Hz、14 位：舵机
pwm::write(7, 1228)?;                  // 占空比 0 到 2^14-1
```

输出脚后面的数字是安全电平：设备启动后、应用的代码运行之前，应用停止、出故障、被替换之后，宿主都把引脚保持在这个电平，`pwm` 引脚保持低电平。应用不用自己在 `init` 里「先把输出关掉」，也不用担心出故障时输出停在半途。每种芯片能用哪些引脚见[应用包](../../docs/design/app-package.md#引脚)。

**接了执行机构的输出用 `gpio::pulse`。** 「按住才动、松开两秒内必须停」这类要求不能靠应用自己的定时器去关输出：应用只有一个任务，它在发布消息时可能要等一次卡住的网络操作（最长十几秒），这期间定时器事件送不到应用手里。`gpio::pulse` 的时限由宿主自己的定时器执行，到时直接把引脚置回安全电平，不经过应用；应用随后收到 `Event::PinExpired`，用来更新自己的状态。再次调用是续期，`gpio::write` 取消时限。

## 本机测试和运行

改业务不用碰设备：逻辑用单元测试测，整个应用在本机的模拟器里跑。

**单元测试**。把业务逻辑（协议解析、状态变化）写成不碰宿主接口的函数，测试写在同一个文件的 `#[cfg(test)]` 模块里。工程默认编译到 WASM，`app test` 替你把测试编译到本机再跑（`--` 之后的内容原样交给 `cargo test`）。本机上没有宿主，碰到宿主接口的代码一调用就 panic。

**本机运行**。`app run` 运行的是编译好的 `.wasm`，用的是设备上的同一份宿主核心加 WAMR 的本机构建，所以清单写错、引脚在这个芯片上不能用、碰了没有声明的引脚、死循环、陷阱，在本机就能看到。时间是虚拟的：定时器只在你推进时钟时到期，应用睡眠直接把时钟拨快，结果是确定的。

```console
$ python3 tools/esp_base.py app run ../my-app --param site=lab
my_app 0.1.0+0：wasm 431 字节，内存上限 12288 字节，芯片 esp32c3
[       0 ms] log info: my-app: init
[       0 ms] connect keepalive_s=30, will=null
[       0 ms] timer id=0, delay_ms=1000, period_ms=1000
up                                  ← 业务 MQTT 连上了
adv 1000                            ← 时钟推进 1 秒
[    1000 ms] publish topic=my-app/tick, payload=1, qos=0, retain=false, will=false
```

命令：`up`、`down`（业务 MQTT 连上、断开），`msg <主题> [载荷]`（送一条消息给应用），`adv <毫秒>`，`in <引脚> <0|1>`（给输入脚加电平），`clock <Unix 毫秒>`（对时），`start`、`stop`、`stats`、`quit`。`--script <文件>` 从文件读命令；应用启动失败或出过故障时退出码是 1。

要自动核对结果，用 Python 驱动模拟器（`tools/esp_base_sim.py`），配合 `unittest`：

```python
from esp_base_sim import Simulator

with Simulator("target/esp-base/hxh_lift.ebapp", params={"hxh_site": "lab"}) as sim:
    sim.start()
    sim.mqtt_up()
    sim.message("hxh-studio/lab/lift/cmd/move", {"direction": "up"})
    assert sim.gpio[21] == 1          # 输出脚的电平
    sim.advance(2000)                 # 没有续租：推杆自己停下
    assert sim.gpio[21] == 0
    assert sim.published[-1].json()["motion"] == "stop"
```

每个命令返回它期间发生的事件；`sim.gpio`、`sim.pwm`、`sim.published`、`sim.logs`、`sim.subscriptions`、`sim.timers`、`sim.will`、`sim.fault` 是累计下来的状态。与设备的差别：不验签；本机的指针是 64 位，运行时自己的结构比设备上大三成左右，所以模拟器给的内存池是清单 `memory` 的 1.5 倍，`stats` 报的也是本机的数字——设备上的实际用量看 `app/state` 里的 `pool_used`、`pool_peak`；PWM 只核对这个频率配不配得出这样的分辨率，不模拟波形。

## 约束

- **体积就是内存。** ESP32-C3 上应用整体载入内存，每大 1 KB 就多占 1 KB。不要用 `format!`、`core::fmt` 和带格式的 `panic!`；数字转字符串自己写（见样例）。
- **没有堆。** 状态放在应用结构体里，缓冲用定长数组。
- **每次调用有指令预算。** 死循环或算得太久的调用会被宿主停掉，应用随后被重启。长时间的事拆成定时器事件。
- **睡眠是短的。** 应用睡着时事件都在排队，所以一次调用（`init` 或一个事件）里累计最多睡 1000 毫秒，超出的那次返回 `Error::TooLarge`、不睡。换向前停 200 毫秒这样的事可以睡，更长的等待用定时器。
- **事件会迟到，不会乱序。** 定时器事件不保证准点：应用正在处理别的事件、或卡在一次网络操作里时，它会晚到；同一个定时器在应用来得及处理之前多次到期只算一次。业务 MQTT 的消息在应用处理不过来时会被丢弃（宿主只为它排队有限的几条）。
- **`match` 里留一个 `_`。** 以后的固件会有新的事件和错误码，`Event` 和 `Error` 都不能穷举匹配。
- **出错即陷阱。** `panic`、越界、把线性内存之外的指针交给宿主接口，都会让应用停止并重启；系统层不受影响。
- **清单写在 `Cargo.toml` 里。** `[package.metadata.esp-base]` 的 `memory` 是应用的内存上限（运行时内存池，字节），`stack` 是操作数栈；可选的 `chips` 限定能装的芯片，`[package.metadata.esp-base.pins]` 声明引脚。
- **链接参数不能改。** `.cargo/config.toml` 把栈设为 4 KiB 并导出 `__heap_base`、`__data_end`，宿主据此把线性内存收缩到实际用量；没有收缩的应用会被拒绝。

## 测试夹具

改了 SDK、样例或 `fixtures` 之后重新生成宿主测试用的应用，并把产物一起提交：

```bash
./build_fixtures.sh                                # 写到 ../../firmware/tests/apps/
bash ../../firmware/tests/run_wasm_host_tests.sh   # 用这些应用跑宿主测试
python3 ../sim/test_sim.py                         # 模拟器、它的 Python 封装和 app new／build／test／run
```
