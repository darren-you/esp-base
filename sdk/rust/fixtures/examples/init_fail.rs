//! 夹具：启动失败。
#![no_std]
use esp_base_app::{App, Error, Event};

struct InitFail;

impl App for InitFail {
    fn init() -> Result<Self, Error> {
        Err(Error::Invalid)
    }
    fn event(&mut self, _: Event) {}
}

esp_base_app::app!(InitFail);
