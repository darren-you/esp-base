//! 夹具：把线性内存之外的指针交给宿主接口，宿主应当拒绝并停掉应用，而不是读越界。
#![no_std]
use esp_base_app::{sys, App, Error, Event};

struct BadPointer;

impl App for BadPointer {
    fn init() -> Result<Self, Error> {
        Ok(BadPointer)
    }
    fn event(&mut self, _: Event) {
        unsafe { sys::log(3, 0x00F0_0000 as *const u8, 16) };
    }
}

esp_base_app::app!(BadPointer);
