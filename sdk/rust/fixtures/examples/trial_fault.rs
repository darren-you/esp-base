//! 夹具：启动正常，半秒后的第一个定时器事件里死循环。用来核对「试运行期间出故障就回到上一版」。
#![no_std]
use esp_base_app::{timer, App, Error, Event};

struct TrialFault;

impl App for TrialFault {
    fn init() -> Result<Self, Error> {
        timer::set(0, 500, 0)?;
        Ok(TrialFault)
    }
    fn event(&mut self, _: Event) {
        let mut counter = 0u32;
        loop {
            counter = counter.wrapping_add(1);
            core::hint::black_box(counter);
        }
    }
}

esp_base_app::app!(TrialFault);
