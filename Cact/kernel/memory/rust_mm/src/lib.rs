#![no_std]
#![allow(internal_features)]
// Safety baseline: implicit unsafe ops inside `unsafe fn` bodies are rejected, and
// every `unsafe` block must carry a SAFETY comment (enforced under clippy).
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unused_unsafe)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(clippy::missing_safety_doc)]
#![deny(static_mut_refs)]
#![deny(improper_ctypes, improper_ctypes_definitions)]
// Additional safety lints (all verified zero-hit when enabled, 2026-09-26):
// transmute misuse, byte-vs-element count confusion and assumptions that
// uninitialised memory is valid become hard errors instead of silent warnings.
#![deny(clippy::transmute_ptr_to_ref)]
#![deny(clippy::transmute_ptr_to_ptr)]
#![deny(clippy::useless_transmute)]
#![deny(clippy::size_of_in_element_count)]
#![deny(clippy::uninit_assumed_init)]
#![deny(invalid_reference_casting)]
// Finally, at most one unsafe operation per `unsafe` block, so every block is
// small enough to audit on its own (the same maximal tightening applied to the
// scheduler and the network stack).
#![deny(clippy::multiple_unsafe_ops_per_block)]

//! Rust-side memory management: physical allocator, kernel heap, page tables, COW,
//! page faults, mmap, and per-process helpers. Calls into C for low-level VMM/PMM.

mod ffi;
// Rust API for other kernel crates: the same functions the C ABI exports, but
// reachable as ordinary Rust items, so `sched`/`rust_drm`/`rust_net` call them
// with `use cact_mm::…` instead of re-declaring `extern "C"` themselves.
pub use crate::alloc::heap::{kfree, kmalloc, kmalloc_aligned};
pub use crate::pmm::{free_page, kalloc};
pub use crate::process::proc_mm::{proc_free_pages, proc_tracker_add};
pub use crate::process::shm::shm_detach_all;
pub use crate::vmm::mmap::{mmap_table_clone, mmap_table_free, mmap_table_init};
pub use crate::vmm::cow::vmm_fork_address_space;
pub use crate::vmm::paging::{
    vmm_create_address_space, vmm_free_address_space, vmm_map,
    vmm_sync_kernel_mmio_mappings,
};
mod safe;
pub mod pmm;
pub mod alloc;
pub mod vmm;
pub mod fault;
pub mod process;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    crate::safe::kprint_str(c"[RUST] PANIC\n".as_ptr() as *const u8);
    // SAFETY: panicking CPU, unrecoverable state; `cli` touches no memory and clearing IF stops
    // the interrupt storm while the CPU parks below.
    unsafe {
        core::arch::asm!("cli", options(nomem, nostack));
    }
    loop {
        // SAFETY: hlt is the only safe way to spin in a kernel panic.
        unsafe {
            core::arch::asm!("hlt", options(nomem, nostack));
        }
    }
}
