#![no_std]
#![allow(internal_features)]
#![allow(static_mut_refs)]

//! Rust-side memory management: physical allocator, kernel heap, page tables, COW,
//! page faults, mmap, and per-process helpers. Calls into C for low-level VMM/PMM.

mod ffi;
mod safe;
pub mod pmm;
pub mod alloc;
pub mod vmm;
pub mod fault;
pub mod process;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    crate::safe::kprint_str(b"[RUST] PANIC\n\0".as_ptr());
    loop {
        // SAFETY: hlt is the only safe way to spin in a kernel panic.
        unsafe {
            core::arch::asm!("hlt", options(nomem, nostack));
        }
    }
}
