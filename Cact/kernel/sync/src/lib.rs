//! Kernel synchronization primitives for Cact (`cact_sync`).
//!
//! Provides spinlocks, IRQ-aware spinlocks, mutexes, and counting semaphores that
//! cooperate with the scheduler (`schedule`, MLFQ enqueue) when a contended lock
//! must sleep. Types such as [`task_abi::TaskStruct`] mirror the C ABI and are
//! validated from the `sched` crate via compile-time offset checks.

#![no_std]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(clippy::missing_safety_doc)]

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
