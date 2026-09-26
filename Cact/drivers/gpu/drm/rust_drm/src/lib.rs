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
// The driver-facing ABI carries `*mut drm_device` / `*mut drm_file`, whose Rust
// types own `Vec`s.  A driver only ever holds those as opaque pointers, so the
// pointee layout never crosses the boundary — which is exactly what this lint
// cannot see.  Those types no longer travel through `extern "C"` declarations
// of functions that this crate itself defines (they are called by crate path),
// so the lint is denied: anything it reports is a real ABI boundary and needs a
// local `#[allow]` with a reason.
#![deny(improper_ctypes, improper_ctypes_definitions)]
// Safety baseline: unsafe ops inside `unsafe fn` bodies must be explicit, and every
// `unsafe` block needs a SAFETY comment (the latter enforced under clippy).
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unused_unsafe)]
#![deny(static_mut_refs)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(clippy::missing_safety_doc)]
// Additional safety lints (all verified zero-hit when enabled, 2026-09-26):
// transmute misuse, byte-vs-element count confusion and assumptions that
// uninitialised memory is valid become hard errors instead of silent warnings.
#![deny(clippy::transmute_ptr_to_ref)]
#![deny(clippy::transmute_ptr_to_ptr)]
#![deny(clippy::useless_transmute)]
#![deny(clippy::size_of_in_element_count)]
#![deny(clippy::uninit_assumed_init)]
#![deny(invalid_reference_casting)]
// At most one unsafe operation per `unsafe` block, so every block is small
// enough to audit on its own (the same maximal tightening applied to the
// scheduler, the network stack and the memory manager).
#![deny(clippy::multiple_unsafe_ops_per_block)]

extern crate alloc;

// This crate's `Vec`/`BTreeMap` storage comes from the kernel heap, whose
// single `#[global_allocator]` now lives in the link crate `cact_kernel`.

// The kernel Rust graph's single `#[global_allocator]` lives in the link
// crate (`cact_kernel`); a crate graph may define exactly one.

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

// The kernel Rust graph's single `#[panic_handler]` lives in `cact_mm`, which
// this crate depends on, so one is not defined here.
