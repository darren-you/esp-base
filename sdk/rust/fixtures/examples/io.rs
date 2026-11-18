//! 夹具：把 MQTT 消息当命令执行，结果发布到 io/result（十进制的返回值）或 io/text（读到的文本）。host 测试用它
//! 逐个核对引脚、PWM、业务参数、设备信息和睡眠这些宿主接口。命令的第一个字节是操作，后面是小端的 32 位参数
//! （参数原样交给宿主接口，所以也能传不合法的值）；业务参数的键直接跟在操作字节后面。
#![no_std]
use esp_base_app::{device, gpio, mqtt, param, pwm, sleep_ms, sys, App, Error, Event};

struct Io;

fn word(data: &[u8], index: usize) -> u32 {
    let at = 1 + 4 * index;
    if data.len() < at + 4 {
        return 0;
    }
    u32::from_le_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]])
}

fn report(code: i32) {
    let mut digits = [0u8; 12];
    let mut index = digits.len();
    let mut value = code.unsigned_abs();
    loop {
        index -= 1;
        digits[index] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    if code < 0 {
        index -= 1;
        digits[index] = b'-';
    }
    let _ = mqtt::publish("io/result", &digits[index..], mqtt::Qos::AtMostOnce, false);
}

fn text(result: Result<&[u8], Error>) {
    match result {
        Ok(data) => {
            let _ = mqtt::publish("io/text", data, mqtt::Qos::AtMostOnce, false);
        }
        Err(error) => report(error.code()),
    }
}

/// SDK 的封装走一遍：推杆式的输出脚、舵机式的 PWM 脚。返回出错的步数。
fn wrappers() -> i32 {
    let mut failures = 0;
    failures += gpio::write(4, true).is_err() as i32;
    failures += (gpio::read(4) != Ok(true)) as i32;
    failures += gpio::write(4, false).is_err() as i32;
    failures += (gpio::write(9, true) != Err(Error::Denied)) as i32;
    failures += pwm::config(7, 50, 14).is_err() as i32;
    failures += pwm::write(7, 1228).is_err() as i32;
    failures += sleep_ms(40).is_err() as i32;
    failures += (pwm::read(7) != Ok(1228)) as i32;
    failures
}

impl App for Io {
    fn init() -> Result<Self, Error> {
        mqtt::connect(None, 30)?;
        mqtt::subscribe("io/cmd", mqtt::Qos::AtLeastOnce)?;
        Ok(Io)
    }

    fn event(&mut self, event: Event) {
        let Event::MqttMessage(message) = event else { return };
        let mut buffer = [0u8; 64];
        let Ok(data) = message.payload(&mut buffer) else { return };
        if data.is_empty() {
            return;
        }
        let (a, b, c) = (word(data, 0), word(data, 1), word(data, 2));
        match data[0] {
            1 => report(unsafe { sys::gpio_write(a, b) }),
            2 => report(unsafe { sys::gpio_read(a) }),
            3 => report(unsafe { sys::pwm_config(a, b, c) }),
            4 => report(unsafe { sys::pwm_write(a, b) }),
            5 => report(unsafe { sys::pwm_read(a) }),
            6 => {
                let mut value = [0u8; 16];
                let key = unsafe { core::str::from_utf8_unchecked(&data[1..]) };
                text(param(key, &mut value));
            }
            7 => {
                let mut value = [0u8; 40];
                text(if a == 0 { device::id(&mut value) } else { device::chip(&mut value) });
            }
            8 => report(unsafe { sys::info(a, core::ptr::null_mut(), 0) }),
            9 => {
                // 连睡 b 次、每次 a 毫秒：返回第一个错误。
                let mut result = 0;
                for _ in 0..b {
                    if let Err(error) = sleep_ms(a) {
                        result = error.code();
                        break;
                    }
                }
                report(result);
            }
            10 => report(wrappers()),
            _ => {}
        }
    }
}

esp_base_app::app!(Io);
