//! The DRM/KMS core — entirely Rust.
//!
//! This crate owns the whole core: the device and object model, GEM, KMS,
//! events, properties and every ioctl handler, plus the VFS/devfs glue that
//! publishes `/dev/dri`.  What is left in C next to it is only the interface to
//! something else:
//!
//!   * `drm_drv.h` + `uapi/` — the ABI a hardware driver module compiles
//!     against (that driver is a separate C `.cctk`, so the header must stay
//!     C-parseable), and
//!   * the kernel itself — `kmalloc`, memfd, spinlocks and the VFS/devfs entry
//!     points, reached through `extern "C"` declarations.
//!
//! Object pools are allocator-backed (`alloc`), so nothing in the core is
//! capped: a device may hold any number of CRTCs, framebuffers or properties,
//! and a client any number of handles and events.  The `#[global_allocator]`
//! comes from the kernel's `rust_net` crate, so no second one is defined here.

#![no_std]
#![allow(static_mut_refs)]
// The driver-facing ABI carries `*mut drm_device` / `*mut drm_file`, whose Rust
// types own `Vec`s.  A driver only ever holds those as opaque pointers, so the
// pointee layout never crosses the boundary — which is exactly what this lint
// cannot see.
#![allow(improper_ctypes, improper_ctypes_definitions)]

extern crate alloc;

/// The core's `Vec`/`BTreeMap` storage comes from the kernel heap.
///
/// A `staticlib` that uses `alloc` has to name an allocator, so this crate
/// carries the same `kmalloc_aligned`/`kfree` shim the other kernel Rust
/// libraries do; at link time the archives provide one `__rust_alloc` between
/// them.
struct CactAllocator;

// SAFETY: the kernel heap never returns a pointer that overlaps a live
// allocation, and `kfree` accepts exactly what `kmalloc_aligned` returned.
unsafe impl core::alloc::GlobalAlloc for CactAllocator {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        unsafe extern "C" {
            fn kmalloc_aligned(size: usize, align: u32) -> *mut core::ffi::c_void;
        }
        kmalloc_aligned(layout.size(), layout.align() as u32) as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: core::alloc::Layout) {
        unsafe extern "C" {
            fn kfree(ptr: *mut core::ffi::c_void);
        }
        kfree(ptr as *mut core::ffi::c_void);
    }
}

#[global_allocator]
static ALLOCATOR: CactAllocator = CactAllocator;

mod device;
mod devfs;
mod event;
mod ffi;
mod file;
mod gem;
mod ioctl;
mod mode;
mod structs;
mod syncobj;
mod vfs;

/// The KMS half of the core.  Its sources live in their own directory
/// (`Cact/drivers/gpu/kms/src`) and are pulled into this crate with `#[path]`
/// rather than built as a second crate: both halves share `DrmDevice`/
/// `DrmFile`, and two Rust staticlibs would duplicate those symbols at link
/// time.
#[path = "../../../kms/src/mod.rs"]
mod kms;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // SAFETY: a static NUL-terminated byte string.
    unsafe { ffi::printk(crate::ffi::PANIC_MSG.as_ptr()) };
    loop {
        // SAFETY: hlt is the only safe way to spin in a kernel panic.
        unsafe { core::arch::asm!("hlt", options(nomem, nostack)); }
    }
}
