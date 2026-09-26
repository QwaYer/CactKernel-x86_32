//! Weak link boundary into the scheduler and console.
//!
//! Declares `extern "C"` items (`schedule`, `printk`,
//! `current_task`, `scheduler_lock`) that `cact_sync` references; the final kernel or
//! `sched` staticlib supplies the definitions. Everything crossing this edge
//! (`TaskStruct`, `irq_spinlock_t`) is `#[repr(C)]`, so the boundary is FFI-safe.

use core::ptr;

use crate::spinlock::irq_spinlock_t;
use crate::task_abi::{TaskState, TaskStruct};

unsafe extern "C" {
    fn schedule();
    fn sched_mlfq_wake_task_locked(task: *mut TaskStruct);
    fn printk(s: *const u8);
    static mut current_task: *mut TaskStruct;
    #[link_name = "scheduler_lock"]
    static mut SCHEDULER_LOCK: irq_spinlock_t;
}

#[inline]
pub(crate) fn schedule_yield() {
    // SAFETY: `schedule` is the scheduler entry point supplied by `sched`; it
    // takes no arguments and may be called from any task context. There is no
    // argument to validate.
    unsafe { schedule() }
}

/// State-aware wake (unlinks a Sleeping task from the sleep queue first).
/// Requires [`SCHEDULER_LOCK`] to be held by the caller.
#[inline]
pub(crate) fn mlfq_wake_locked(task: *mut TaskStruct) {
    // SAFETY: caller holds SCHEDULER_LOCK (documented precondition) and passes a
    // task pointer that the scheduler owns, so the enqueue touches only valid
    // scheduler state.
    unsafe { sched_mlfq_wake_task_locked(task) }
}

#[inline]
pub(crate) fn kprint_str(p: *const u8) {
    // SAFETY: callers pass a pointer to a static, NUL-terminated byte string, so
    // `printk`'s scan for the terminator stays in bounds.
    unsafe { printk(p) }
}

#[inline]
pub(crate) fn current_task_ptr() -> *mut TaskStruct {
    // SAFETY: `current_task` is a `static mut` owned by the scheduler; reading
    // its pointer value is atomic on x86 and does not form a reference, so it
    // cannot alias anything.
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
    // SAFETY: `SCHEDULER_LOCK` is a single `static mut` object that is never
    // moved or freed, so the pointer is valid and aligned for `'static`. The
    // caller contract (see # Safety) ensures at most one live `&mut` at a time.
    unsafe { &mut *ptr::addr_of_mut!(SCHEDULER_LOCK) }
}

#[inline]
pub(crate) fn task_state_set(t: *mut TaskStruct, st: TaskState) {
    if !t.is_null() {
        // SAFETY: the null check above rules out the dangling case and callers
        // pass a live task pointer; `state` is a plain enum field, so writing it
        // is in bounds and properly aligned.
        unsafe {
            (*t).state = st;
        }
    }
}
