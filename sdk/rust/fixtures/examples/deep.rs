//! 夹具：无限递归，耗尽应用的栈；宿主应当得到一次陷阱，而不是自己的任务栈溢出。
#![no_std]
use esp_base_app::{App, Error, Event};

struct Deep;

#[inline(never)]
fn descend(depth: u32, pad: &mut [u8; 64]) -> u32 {
    pad[(depth % 64) as usize] = depth as u8;
    let mut next = [0u8; 64];
    descend(core::hint::black_box(depth + 1), &mut next) + pad[0] as u32
}

impl App for Deep {
    fn init() -> Result<Self, Error> {
        Ok(Deep)
    }
    fn event(&mut self, _: Event) {
        let mut pad = [0u8; 64];
        core::hint::black_box(descend(0, &mut pad));
    }
}

esp_base_app::app!(Deep);
