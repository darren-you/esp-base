//! 夹具：处理事件时死循环，宿主应当按指令预算把它停下来。
#![no_std]
use esp_base_app::{App, Error, Event};

struct Spin;

impl App for Spin {
    fn init() -> Result<Self, Error> {
        Ok(Spin)
    }
    fn event(&mut self, _: Event) {
        let mut counter = 0u32;
        loop {
            counter = counter.wrapping_add(1);
            core::hint::black_box(counter);
        }
    }
}

esp_base_app::app!(Spin);
