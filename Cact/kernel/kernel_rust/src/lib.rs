//! The kernel's Rust link crate.
//!
//! Every kernel Rust crate is an `rlib` so that Rust code can call Rust code
//! with plain `use` (`cact_mm::kfree`, `sched::task_fork`, …) — `extern "C"`
//! is reserved for the real C kernel (`printk`, VFS, ELF loader, APIC, …).
//!
//! Something still has to be a `staticlib`, because meson links one archive
//! into `kernel.bin`.  That is this crate: it depends on the whole Rust graph
//! and pulls every crate into a single archive, so each symbol — and each
//! `#[no_mangle]` C-ABI entry point — exists exactly once in the final image.
//!
//! The graph's `#[panic_handler]` and `#[global_allocator]` live in `cact_mm`
//! and `cact_net` respectively; a crate graph may define each exactly once.

#![no_std]

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};

/// The kernel's global allocator: `cact_mm`'s heap, with alignment support.
struct CactAllocator;

// SAFETY: every method upholds `GlobalAlloc`'s contract; `kmalloc_aligned`
// returns either null or a block of exactly `layout.size()` bytes aligned to
// `layout.align()`, and `kfree` releases exactly such a block.
unsafe impl GlobalAlloc for CactAllocator {
    /// # Safety
    ///
    /// See [`GlobalAlloc::alloc`]: `layout` must have a non-zero size.
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // The `GlobalAlloc` contract makes `layout.size()` non-zero and
        // `layout.align()` a power of two, which is what `kmalloc_aligned`
        // requires; the returned block belongs to the caller.  The call itself
        // is safe (allocation has no preconditions).
        cact_mm::kmalloc_aligned(layout.size() as u32, layout.align() as u32)
    }

    /// # Safety
    ///
    /// See [`GlobalAlloc::dealloc`]: `ptr` must come from `alloc` on this
    /// allocator, with the same `layout`, and must not have been freed already.
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        // SAFETY: the caller upholds `GlobalAlloc::dealloc`'s contract, so `ptr`
        // is a live block from `kmalloc_aligned` that `kfree` may release.
        unsafe { cact_mm::kfree(ptr) };
    }
}

#[global_allocator]
static ALLOCATOR: CactAllocator = CactAllocator;

pub use cact_drm as drm;
pub use cact_hmac_ffi as hmac;
pub use cact_mm as mm;
pub use cact_net as net;
pub use sched as sched;
