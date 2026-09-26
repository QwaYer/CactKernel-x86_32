//! Kernel synchronization primitives for Cact (`cact_sync`).
//!
//! Provides spinlocks, IRQ-aware spinlocks, mutexes, and counting semaphores that
//! cooperate with the scheduler (`schedule`, MLFQ enqueue) when a contended lock
//! must sleep. Types such as [`task_abi::TaskStruct`] mirror the C ABI and are
//! validated from the `sched` crate via compile-time offset checks.

#![no_std]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
// Safety baseline: unsafe ops inside `unsafe fn` bodies must be explicit, and every
// `unsafe` block needs a SAFETY comment (the latter enforced under clippy).
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
// Maximal-safety: one unsafe operation per `unsafe` block, so each `// SAFETY:`
// justifies exactly one operation.
#![deny(clippy::multiple_unsafe_ops_per_block)]

pub mod kernel_types;
pub mod task_abi;

mod hal;
mod mutex;
mod sched_link;
mod semaphore;
mod spinlock;

pub use kernel_types::{MmapTable, ProcPageTracker, TaskFdTable, VfsNode};
pub use mutex::*;
pub use semaphore::*;
pub use spinlock::*;
pub use task_abi::{ProcMeta, TaskShmAttach, TaskState, TaskStruct, NSIG, TASK_SHM_MAX};
