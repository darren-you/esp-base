//! 夹具：处理定时器事件时越界访问，触发 panic（陷阱）；其他事件正常。
#![no_std]
use esp_base_app::{log, App, Error, Event};

struct Trap {
    values: [u8; 4],
}

impl App for Trap {
    fn init() -> Result<Self, Error> {
        Ok(Trap { values: [1, 2, 3, 4] })
    }
    fn event(&mut self, event: Event) {
        match event {
            Event::Timer(id) => {
                let index = core::hint::black_box(id as usize + 4);
                self.values[index] = 0; // 越界
            }
            _ => log::info("trap: alive"),
        }
    }
}

esp_base_app::app!(Trap);
