#![no_std]
#![allow(internal_features)]
#![allow(static_mut_refs)]

mod ffi;
pub mod pmm;
pub mod alloc;
pub mod vmm;
pub mod fault;
pub mod process;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe {
        ffi::kprint(b"[RUST] PANIC\n\0".as_ptr());
    }
    loop {
        unsafe {
            core::arch::asm!("hlt", options(nomem, nostack));
        }
    }
}