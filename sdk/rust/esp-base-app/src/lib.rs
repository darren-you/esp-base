// SPDX-License-Identifier: Apache-2.0
//! ESP Base 业务应用 SDK。
//!
//! 业务编译成 `wasm32v1-none` 的 WASM 模块，由 esp-base 固件里的 WASM 宿主加载运行。应用是事件驱动的：
//! 宿主调用一次 `app_init`，之后每个事件调用一次 `app_event`，同一时刻只有一次调用在执行，所以应用里
//! 没有线程，也不需要锁。应用只能经这里的宿主接口接触设备，拿不到 Wi-Fi 和 MQTT 的账户密码。
//!
//! 用法：实现 [`App`]，再用 [`app!`] 导出入口。
//!
//! 体积就是内存：ESP32-C3 上应用整体载入内存，所以这里不用 `core::fmt`，出错直接陷阱。
#![no_std]

/// 宿主接口的原始声明：导入模块 `base`。指针都是应用线性内存里的偏移，由宿主核对范围。
pub mod sys {
    #[link(wasm_import_module = "base")]
    extern "C" {
        pub fn log(level: u32, text: *const u8, length: u32);
        pub fn uptime_ms() -> u64;
        pub fn unix_ms() -> u64;
        pub fn timer_set(id: u32, delay_ms: u32, period_ms: u32) -> i32;
        pub fn timer_cancel(id: u32) -> i32;
        pub fn sleep_ms(ms: u32) -> i32;
        pub fn mqtt_connect(will_topic: *const u8, will_topic_length: u32, will_payload: *const u8,
                            will_payload_length: u32, will_qos: u32, will_retain: u32, keepalive_s: u32) -> i32;
        pub fn mqtt_disconnect() -> i32;
        pub fn mqtt_subscribe(filter: *const u8, length: u32, qos: u32) -> i32;
        pub fn mqtt_publish(topic: *const u8, topic_length: u32, payload: *const u8, payload_length: u32, qos: u32,
                            retain: u32) -> i32;
        pub fn event_topic(buffer: *mut u8, capacity: u32) -> i32;
        pub fn event_payload(buffer: *mut u8, capacity: u32) -> i32;
        pub fn param(key: *const u8, key_length: u32, buffer: *mut u8, capacity: u32) -> i32;
        pub fn info(what: u32, buffer: *mut u8, capacity: u32) -> i32;
        pub fn gpio_write(pin: u32, level: u32) -> i32;
        pub fn gpio_read(pin: u32) -> i32;
        pub fn pwm_config(pin: u32, frequency_hz: u32, resolution_bits: u32) -> i32;
        pub fn pwm_write(pin: u32, duty: u32) -> i32;
        pub fn pwm_read(pin: u32) -> i32;
    }
}

/// 宿主接口的错误。
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// 参数不合法。
    Invalid,
    /// 清单没有声明，或属于系统。
    Denied,
    /// 还没连上、还没对时。
    NotReady,
    /// 内存或队列不够。
    NoMemory,
    /// 超出长度上限，或缓冲太小。
    TooLarge,
    /// 没有这个键。
    NotFound,
    /// 这个 SDK 不认识的错误码（固件比 SDK 新）。
    Other(i32),
}

impl Error {
    /// 宿主接口的原始错误码（负数），写日志时用。
    pub fn code(self) -> i32 {
        match self {
            Error::Invalid => -1,
            Error::Denied => -2,
            Error::NotReady => -3,
            Error::NoMemory => -4,
            Error::TooLarge => -5,
            Error::NotFound => -6,
            Error::Other(code) => code,
        }
    }
}

fn check(code: i32) -> Result<usize, Error> {
    match code {
        0.. => Ok(code as usize),
        -1 => Err(Error::Invalid),
        -2 => Err(Error::Denied),
        -3 => Err(Error::NotReady),
        -4 => Err(Error::NoMemory),
        -5 => Err(Error::TooLarge),
        -6 => Err(Error::NotFound),
        other => Err(Error::Other(other)),
    }
}

/// 日志：出现在设备的串口日志里，也可以远程读取最近的若干行。
pub mod log {
    use super::sys;

    pub fn error(text: &str) {
        unsafe { sys::log(1, text.as_ptr(), text.len() as u32) }
    }
    pub fn warn(text: &str) {
        unsafe { sys::log(2, text.as_ptr(), text.len() as u32) }
    }
    pub fn info(text: &str) {
        unsafe { sys::log(3, text.as_ptr(), text.len() as u32) }
    }
}

/// 开机以来的毫秒数，单调递增。
pub fn uptime_ms() -> u64 {
    unsafe { sys::uptime_ms() }
}

/// UTC 时间（Unix 毫秒）；设备还没对时时为 `None`。
pub fn unix_ms() -> Option<u64> {
    match unsafe { sys::unix_ms() } {
        0 => None,
        now => Some(now),
    }
}

/// 让应用任务睡一会儿；期间到来的事件排队等候。一次调用（`init` 或一个事件）里累计最多睡 1000 毫秒，
/// 超出时返回 [`Error::TooLarge`]、不睡：更长的等待用定时器。
pub fn sleep_ms(ms: u32) -> Result<(), Error> {
    check(unsafe { sys::sleep_ms(ms) }).map(|_| ())
}

/// 业务参数：出厂资料里 `business` 对象的字符串值（最长 255 字节），按键读进 `buffer`。键是最多 15 个字符的
/// 小写字母、数字、下划线。没有这个键时返回 [`Error::NotFound`]，缓冲太小时返回 [`Error::TooLarge`]。
pub fn param<'a>(key: &str, buffer: &'a mut [u8]) -> Result<&'a [u8], Error> {
    let length =
        check(unsafe { sys::param(key.as_ptr(), key.len() as u32, buffer.as_mut_ptr(), buffer.len() as u32) })?;
    Ok(&buffer[..length])
}

/// 设备信息。
pub mod device {
    use super::{check, sys, Error};

    fn read(what: u32, buffer: &mut [u8]) -> Result<&[u8], Error> {
        let length = check(unsafe { sys::info(what, buffer.as_mut_ptr(), buffer.len() as u32) })?;
        Ok(&buffer[..length])
    }

    /// 设备 UUID（36 字节的小写文本）。
    pub fn id(buffer: &mut [u8]) -> Result<&[u8], Error> {
        read(0, buffer)
    }

    /// 芯片型号：`esp32c3`、`esp32`。
    pub fn chip(buffer: &mut [u8]) -> Result<&[u8], Error> {
        read(1, buffer)
    }
}

/// 数字引脚。引脚要先在应用清单的 `pins` 里声明用途：`in`、`in_pullup`、`in_pulldown` 只能读；`out0`、`out1`
/// 能写也能读回，后面的数字是安全电平——应用启动前、停止后、出故障时，宿主都把引脚保持在这个电平。
/// 没有声明的引脚一律返回 [`Error::Denied`]。
pub mod gpio {
    use super::{check, sys, Error};

    pub fn write(pin: u8, high: bool) -> Result<(), Error> {
        check(unsafe { sys::gpio_write(pin as u32, high as u32) }).map(|_| ())
    }

    /// 引脚的实际电平。
    pub fn read(pin: u8) -> Result<bool, Error> {
        check(unsafe { sys::gpio_read(pin as u32) }).map(|level| level != 0)
    }
}

/// PWM 输出。引脚要先在应用清单的 `pins` 里声明为 `pwm`；应用没在运行时这些引脚保持低电平。
pub mod pwm {
    use super::{check, sys, Error};

    /// 设定频率（1—40000 Hz）和分辨率（1—14 位），占空比从 0 开始。频率和分辨率相同的引脚共用一个定时器，
    /// 一个应用最多用 4 种不同的组合。这个频率配不出这样的分辨率时返回 [`Error::Invalid`]。
    pub fn config(pin: u8, frequency_hz: u32, resolution_bits: u8) -> Result<(), Error> {
        check(unsafe { sys::pwm_config(pin as u32, frequency_hz, resolution_bits as u32) }).map(|_| ())
    }

    /// 占空比：0 到 2^分辨率-1，从下一个周期起生效。还没 `config` 时返回 [`Error::NotReady`]。
    pub fn write(pin: u8, duty: u32) -> Result<(), Error> {
        check(unsafe { sys::pwm_write(pin as u32, duty) }).map(|_| ())
    }

    /// 硬件当前输出的占空比。
    pub fn read(pin: u8) -> Result<u32, Error> {
        check(unsafe { sys::pwm_read(pin as u32) }).map(|duty| duty as u32)
    }
}

/// 定时器：编号 0—3，到期时应用收到 [`Event::Timer`]。
pub mod timer {
    use super::{check, sys, Error};

    /// `delay_ms` 后触发一次；`period_ms` 非 0 时此后按周期触发。重复设置同一个编号会取代原来的。
    pub fn set(id: u8, delay_ms: u32, period_ms: u32) -> Result<(), Error> {
        check(unsafe { sys::timer_set(id as u32, delay_ms, period_ms) }).map(|_| ())
    }

    pub fn cancel(id: u8) {
        unsafe { sys::timer_cancel(id as u32) };
    }
}

/// 业务 MQTT：连接的地址和账户由系统保存、可以远程配置，应用拿不到。宿主负责建立连接、自动重连，
/// 每次连上后重新订阅，并把连接状态和收到的消息作为事件交给应用。
pub mod mqtt {
    use super::{check, sys, Error};

    #[derive(Clone, Copy)]
    pub enum Qos {
        AtMostOnce = 0,
        AtLeastOnce = 1,
    }

    /// 遗嘱：连接意外断开时由 Broker 代发；应用被停止或替换时宿主也会先发出它。
    pub struct Will<'a> {
        pub topic: &'a str,
        pub payload: &'a [u8],
        pub qos: Qos,
        pub retain: bool,
    }

    /// 要求建立业务连接。只需调用一次（通常在 `init` 里）；连上时应用收到 [`super::Event::MqttUp`]。
    pub fn connect(will: Option<Will>, keepalive_s: u16) -> Result<(), Error> {
        let code = match will {
            Some(will) => unsafe {
                sys::mqtt_connect(will.topic.as_ptr(), will.topic.len() as u32, will.payload.as_ptr(),
                                  will.payload.len() as u32, will.qos as u32, will.retain as u32, keepalive_s as u32)
            },
            None => unsafe { sys::mqtt_connect(core::ptr::null(), 0, core::ptr::null(), 0, 0, 0, keepalive_s as u32) },
        };
        check(code).map(|_| ())
    }

    pub fn disconnect() {
        unsafe { sys::mqtt_disconnect() };
    }

    /// 订阅；可以在连上之前调用，宿主会记住。
    pub fn subscribe(filter: &str, qos: Qos) -> Result<(), Error> {
        check(unsafe { sys::mqtt_subscribe(filter.as_ptr(), filter.len() as u32, qos as u32) }).map(|_| ())
    }

    /// 发布；没连上时返回 [`Error::NotReady`]。
    pub fn publish(topic: &str, payload: &[u8], qos: Qos, retain: bool) -> Result<(), Error> {
        check(unsafe {
            sys::mqtt_publish(topic.as_ptr(), topic.len() as u32, payload.as_ptr(), payload.len() as u32, qos as u32,
                              retain as u32)
        })
        .map(|_| ())
    }
}

/// 宿主交给应用的事件。
pub enum Event {
    /// 定时器到期。
    Timer(u8),
    /// 业务 MQTT 连上了。
    MqttUp,
    /// 业务 MQTT 断开了。
    MqttDown,
    /// 收到一条消息；主题和载荷用 [`Message`] 读取。
    MqttMessage(Message),
    /// 这个 SDK 不认识的事件（固件比 SDK 新），忽略即可。
    Unknown(u32),
}

/// 当前这条 MQTT 消息：只在处理这个事件期间有效。
pub struct Message {
    topic_length: u32,
    payload_length: u32,
}

impl Message {
    pub fn topic_len(&self) -> usize {
        self.topic_length as usize
    }

    pub fn payload_len(&self) -> usize {
        self.payload_length as usize
    }

    /// 把主题读进 `buffer`；缓冲太小时返回 [`Error::TooLarge`]。
    pub fn topic<'a>(&self, buffer: &'a mut [u8]) -> Result<&'a [u8], Error> {
        let length = check(unsafe { sys::event_topic(buffer.as_mut_ptr(), buffer.len() as u32) })?;
        Ok(&buffer[..length])
    }

    /// 把载荷读进 `buffer`；缓冲太小时返回 [`Error::TooLarge`]。
    pub fn payload<'a>(&self, buffer: &'a mut [u8]) -> Result<&'a [u8], Error> {
        let length = check(unsafe { sys::event_payload(buffer.as_mut_ptr(), buffer.len() as u32) })?;
        Ok(&buffer[..length])
    }
}

impl Event {
    #[doc(hidden)]
    pub fn from_raw(kind: u32, a: u32, b: u32) -> Self {
        match kind {
            1 => Event::Timer(a as u8),
            2 => Event::MqttUp,
            3 => Event::MqttDown,
            4 => Event::MqttMessage(Message { topic_length: a, payload_length: b }),
            other => Event::Unknown(other),
        }
    }
}

/// 一个业务应用。
pub trait App: Sized {
    /// 应用启动时调用一次：建立初始状态、要求连接、设置定时器。返回 `Err` 表示启动失败，宿主会回退到上一版。
    fn init() -> Result<Self, Error>;
    /// 每个事件调用一次。
    fn event(&mut self, event: Event);
}

/// 导出应用入口：`esp_base_app::app!(MyApp);`
#[macro_export]
macro_rules! app {
    ($app:ty) => {
        static mut __ESP_BASE_APP: ::core::option::Option<$app> = ::core::option::Option::None;

        #[no_mangle]
        pub extern "C" fn app_init() -> i32 {
            match <$app as $crate::App>::init() {
                ::core::result::Result::Ok(app) => {
                    unsafe { *::core::ptr::addr_of_mut!(__ESP_BASE_APP) = ::core::option::Option::Some(app) };
                    0
                }
                ::core::result::Result::Err(_) => 1,
            }
        }

        #[no_mangle]
        pub extern "C" fn app_event(kind: u32, a: u32, b: u32) {
            if let ::core::option::Option::Some(app) = unsafe { &mut *::core::ptr::addr_of_mut!(__ESP_BASE_APP) } {
                $crate::App::event(app, $crate::Event::from_raw(kind, a, b));
            }
        }
    };
}

/// 业务出错时直接陷阱：宿主停掉应用并按策略重启或回退。不带格式化信息，省体积。
#[cfg(target_arch = "wasm32")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}
