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
    /// Per-CPU `current_task` accessor supplied by the `sched` crate.
    fn cact_current_task_get() -> *mut TaskStruct;
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
    // SAFETY: `cact_current_task_get` is the scheduler's per-CPU accessor; it takes no
    // arguments and returns this CPU's live running-task pointer (possibly null).
    unsafe { cact_current_task_get() }
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

/// True when `t` has been marked Zombie — a terminating signal was handled for
/// it while it was running.  A park path must then NOT overwrite the state with
/// `Sleeping`/`Waiting`: that resurrects the task (it gets queued as a sleeper
/// and rescheduled) and it is never reaped, so a process looping on
/// `recvfrom <= 0 → sleep(1)` would ignore Ctrl+C forever.
#[inline]
pub(crate) fn task_is_zombie(t: *mut TaskStruct) -> bool {
    if t.is_null() {
        return false;
    }
    // SAFETY: the null check above rules out the dangling case and callers pass
    // a live task pointer; `state` is a plain enum field, so reading it is in
    // bounds and properly aligned.
    unsafe { matches!((*t).state, TaskState::Zombie) }
}
