#![no_std]
#![allow(internal_features)]
#![allow(static_mut_refs)]

mod ffi;
pub mod pmm;
pub mod heap;
pub mod paging;
pub mod cow;
pub mod slab;
pub mod swap;
pub mod oom;
pub mod page_fault;
pub mod proc_mm;
pub mod mmap;
pub mod shm;

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