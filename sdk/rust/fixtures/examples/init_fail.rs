//! 夹具：启动失败。宿主把 init 返回的错误码记在启动失败的原因里。
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
