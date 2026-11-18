//! 夹具：设备级测试用。启动时把清单里声明的引脚走一遍，每一步的返回值写成一行日志（`pins: out=0 high=1 …`），
//! 之后什么也不做。引脚按芯片取：两种芯片上都避开 Flash、USB、串口和启动时要采样的引脚。
#![no_std]
use esp_base_app::{device, log, sys, App, Error, Event};

struct Pins;

/// 把 ` 名字=数值` 接到 `line` 后面（不用 core::fmt）。
fn append(line: &mut [u8; 128], at: &mut usize, name: &str, value: i32) {
    let mut digits = [0u8; 12];
    let mut index = digits.len();
    let mut rest = value.unsigned_abs();
    loop {
        index -= 1;
        digits[index] = b'0' + (rest % 10) as u8;
        rest /= 10;
        if rest == 0 {
            break;
        }
    }
    if value < 0 {
        index -= 1;
        digits[index] = b'-';
    }
    for part in [b" " as &[u8], name.as_bytes(), b"=", &digits[index..]] {
        line[*at..*at + part.len()].copy_from_slice(part);
        *at += part.len();
    }
}

impl App for Pins {
    fn init() -> Result<Self, Error> {
        let mut chip = [0u8; 16];
        // 输出（安全电平低）、输出（安全电平高）、带上拉的输入、两个 PWM、一个没有声明的引脚。
        let [out0, out1, input, pwm_a, pwm_b, undeclared] = match device::chip(&mut chip)? {
            b"esp32c3" => [4, 5, 6, 7, 10, 9],
            _ => [4, 5, 13, 18, 19, 23],
        };
        let mut line = [0u8; 128];
        line[..5].copy_from_slice(b"pins:");
        let mut at = 5;
        unsafe {
            append(&mut line, &mut at, "safe0", sys::gpio_read(out0));
            append(&mut line, &mut at, "safe1", sys::gpio_read(out1));
            append(&mut line, &mut at, "out", sys::gpio_write(out0, 1));
            append(&mut line, &mut at, "high", sys::gpio_read(out0));
            sys::gpio_write(out0, 0);
            append(&mut line, &mut at, "low", sys::gpio_read(out0));
            append(&mut line, &mut at, "in", sys::gpio_read(input));
            append(&mut line, &mut at, "config", sys::pwm_config(pwm_a, 50, 14));
            append(&mut line, &mut at, "write", sys::pwm_write(pwm_a, 1228));
            sys::sleep_ms(40);
            append(&mut line, &mut at, "duty", sys::pwm_read(pwm_a));
            // 换一组参数（重新分配定时器），第二个引脚用同样的参数（共用定时器）。
            append(&mut line, &mut at, "again", sys::pwm_config(pwm_a, 1000, 10));
            append(&mut line, &mut at, "second", sys::pwm_config(pwm_b, 1000, 10));
            append(&mut line, &mut at, "denied", sys::gpio_write(undeclared, 1));
        }
        log::info(unsafe { core::str::from_utf8_unchecked(&line[..at]) });
        Ok(Pins)
    }

    fn event(&mut self, _: Event) {}
}

esp_base_app::app!(Pins);
