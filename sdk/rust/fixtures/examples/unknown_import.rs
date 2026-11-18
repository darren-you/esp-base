//! 夹具：导入了固件没有的宿主接口（相当于应用要求的接口版本比固件新），安装时就应当被拒绝。
#![no_std]
use esp_base_app::{App, Error, Event};

#[link(wasm_import_module = "base")]
extern "C" {
    fn not_in_this_firmware(value: u32) -> i32;
}

struct UnknownImport;

impl App for UnknownImport {
    fn init() -> Result<Self, Error> {
        unsafe { not_in_this_firmware(1) };
        Ok(UnknownImport)
    }
    fn event(&mut self, _: Event) {}
}

esp_base_app::app!(UnknownImport);
