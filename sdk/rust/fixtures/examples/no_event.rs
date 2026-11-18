//! 夹具：只导出 app_init，没有 app_event。
#![no_std]
use esp_base_app as _;

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    0
}
