#![no_std]
#![feature(sync_unsafe_cell)]
// Safety baseline: unsafe ops inside `unsafe fn` bodies must be explicit, and every
// `unsafe` block needs a SAFETY comment (the latter enforced under clippy).
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unused_unsafe)]
#![deny(static_mut_refs)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(clippy::missing_safety_doc)]
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
// scheduler).
#![deny(clippy::multiple_unsafe_ops_per_block)]

//! In-kernel TCP/IP stack (smoltcp): Ethernet shim, sockets, DNS helpers, and
//! integration hooks for the C networking layer.

extern crate alloc;


// This crate's `Vec`/`BTreeMap` storage comes from the kernel heap, whose
// single `#[global_allocator]` now lives in the link crate `cact_kernel`.

// The kernel Rust graph's single `#[global_allocator]` lives in the link
// crate (`cact_kernel`); a crate graph may define exactly one.

pub mod config;
pub mod dns_resolve;
pub mod ffi;
pub mod ffi_kernel;
pub mod http;
pub mod ping;
pub mod runtime;
pub mod socket;
pub mod stack;
pub mod skb;
pub mod tcp;
pub mod tls;
pub mod types;
pub mod udp;

// The kernel Rust graph's single `#[panic_handler]` lives in `cact_mm`,
// which this crate depends on, so it is not defined here.
