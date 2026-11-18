// SPDX-License-Identifier: Apache-2.0
//! 最小样例：每秒计数一次；业务 MQTT 连上后把计数发布到 hello/ticks，收到 hello/in 上的消息就原样回显到 hello/echo。
#![no_std]

use esp_base_app::{log, mqtt, timer, App, Error, Event};

struct Hello {
    ticks: u32,
    online: bool,
}

/// 十进制写入 `buffer` 末尾，返回起始下标（不用 core::fmt，省体积）。
fn decimal(mut value: u32, buffer: &mut [u8; 10]) -> usize {
    let mut index = buffer.len();
    loop {
        index -= 1;
        buffer[index] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            return index;
        }
    }
}

impl App for Hello {
    fn init() -> Result<Self, Error> {
        log::info("hello: init");
        let will = mqtt::Will { topic: "hello/state", payload: b"offline", qos: mqtt::Qos::AtLeastOnce, retain: true };
        mqtt::connect(Some(will), 30)?;
        mqtt::subscribe("hello/in", mqtt::Qos::AtLeastOnce)?;
        timer::set(0, 1000, 1000)?;
        Ok(Hello { ticks: 0, online: false })
    }

    fn event(&mut self, event: Event) {
        match event {
            Event::Timer(_) => {
                self.ticks += 1;
                if self.online {
                    let mut digits = [0u8; 10];
                    let start = decimal(self.ticks, &mut digits);
                    let _ = mqtt::publish("hello/ticks", &digits[start..], mqtt::Qos::AtMostOnce, false);
                }
            }
            Event::MqttUp => {
                self.online = true;
                let _ = mqtt::publish("hello/state", b"online", mqtt::Qos::AtLeastOnce, true);
            }
            Event::MqttDown => self.online = false,
            Event::MqttMessage(message) => {
                let mut payload = [0u8; 128];
                if let Ok(data) = message.payload(&mut payload) {
                    let _ = mqtt::publish("hello/echo", data, mqtt::Qos::AtMostOnce, false);
                }
            }
            Event::Unknown(_) => {}
        }
    }
}

esp_base_app::app!(Hello);
