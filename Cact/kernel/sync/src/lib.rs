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

pub use kernel_types::{DynCtx, MmapTable, ProcPageTracker, TaskFdTable, VfsNode};
pub use mutex::*;
pub use semaphore::*;
pub use spinlock::*;
pub use task_abi::{TaskShmAttach, TaskState, TaskStruct, NSIG, TASK_SHM_MAX};
