//! Weak link boundary into the scheduler and console.
//!
//! Declares `extern "C"` items (`schedule`, `sched_mlfq_enqueue_locked`, `kprint`,
//! `current_task`, `scheduler_lock`) that `cact_sync` references; the final kernel or
//! `sched` staticlib supplies the definitions. `improper_ctypes` is allowed because
//! `irq_spinlock_t` is a Rust type passed through the C ABI edge.
#![allow(improper_ctypes)]

use core::ptr;

use crate::spinlock::irq_spinlock_t;
use crate::task_abi::{TaskState, TaskStruct};

unsafe extern "C" {
    fn schedule();
    fn sched_mlfq_enqueue_locked(task: *mut TaskStruct, priority: u32);
    fn kprint(s: *const u8);
    static mut current_task: *mut TaskStruct;
    #[link_name = "scheduler_lock"]
    static mut SCHEDULER_LOCK: irq_spinlock_t;
}

#[inline]
pub(crate) fn schedule_yield() {
    unsafe { schedule() }
}

#[inline]
pub(crate) fn mlfq_enqueue(task: *mut TaskStruct, priority: u32) {
    unsafe { sched_mlfq_enqueue_locked(task, priority) }
}

#[inline]
pub(crate) fn kprint_str(p: *const u8) {
    unsafe { kprint(p) }
}

#[inline]
pub(crate) fn current_task_ptr() -> *mut TaskStruct {
    unsafe { current_task }
}

/// Returns `&'static mut` to the global scheduler IRQ spinlock.
///
/// # Safety
///
/// `SCHEDULER_LOCK` is a single kernel object; callers must ensure at most one live
/// `&mut` by always pairing with the real lock acquire/release protocol from `sched`.
#[inline]
pub(crate) fn scheduler_lock_mut() -> &'static mut irq_spinlock_t {
    unsafe { &mut *ptr::addr_of_mut!(SCHEDULER_LOCK) }
}

#[inline]
pub(crate) fn task_state_set(t: *mut TaskStruct, st: TaskState) {
    if !t.is_null() {
        unsafe {
            (*t).state = st;
        }
    }
}

#[inline]
pub(crate) fn task_state_get(t: *mut TaskStruct) -> TaskState {
    unsafe { (*t).state }
}

#[inline]
pub(crate) fn task_priority_get(t: *mut TaskStruct) -> u32 {
    unsafe { (*t).priority }
}
