//! Hierarchical timer wheel (`WHEEL_SIZE` slots) for `sleep_until` timeouts.
//!
//! Tasks chain through `wait_next`. `timer_wheel_tick` runs from the timer path;
//! wakeups enqueue under [`crate::task::SCHEDULER_LOCK`].

use core::cell::SyncUnsafeCell;
use core::ptr;
use crate::task::{TaskStruct, TaskState, SCHEDULER_LOCK};
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::mlfq;

const WHEEL_SIZE: usize = 256;

#[derive(Copy, Clone)]
struct TimerSlot {
    head:  *mut TaskStruct,
    count: u32,
}

impl TimerSlot {
    #[allow(dead_code)]
    const fn empty() -> Self {
        Self { head: ptr::null_mut(), count: 0 }
    }

    fn push(&mut self, task: *mut TaskStruct) {
        if task.is_null() {
            return;
        }
        // SAFETY: `task` is non-null and, per this wheel's contract, a live scheduler task, so
        // reading its `proc` field is in bounds.
        let p = unsafe { (*task).proc };
        if !p.is_null() {
            // SAFETY: `p` is the task's live `ProcMeta` (non-null checked here).
            unsafe { (*p).wait_next = self.head };
        }
        self.head = task;
        self.count += 1;
    }

    fn drain(&mut self) -> *mut TaskStruct {
        let head = self.head;
        self.head  = ptr::null_mut();
        self.count = 0;
        head
    }
}

struct TimerWheel {
    slots:        [TimerSlot; WHEEL_SIZE],
    current_tick: u32,
}

/// Wheel slots hold intrusive lists of sleeping tasks; only accessed with the scheduler
/// lock during tick processing and `sched_sleep_ticks` registration.
// SAFETY: `TimerWheel` holds only raw `*mut TaskStruct` heads whose lists are mutated under
// `SCHEDULER_LOCK` (or during single-threaded init), so sharing the static across threads cannot
// produce an unsynchronised access to the slots.
unsafe impl Sync for TimerWheel {}

impl TimerWheel {
    const fn new() -> Self {
        Self {
            slots:        [TimerSlot::empty(); WHEEL_SIZE],
            current_tick: 0,
        }
    }
}

static SLEEP_WHEEL: SyncUnsafeCell<TimerWheel> = SyncUnsafeCell::new(TimerWheel::new());

#[inline]
fn sleep_wheel_mut() -> *mut TimerWheel {
    SLEEP_WHEEL.get()
}

pub fn timer_wheel_global_init() {
    // SAFETY: called from `task_init` during single-threaded boot, before interrupts or other
    // CPUs exist, so nothing else can observe `SLEEP_WHEEL` while it is reset here.
    let sw = unsafe { &mut *sleep_wheel_mut() };
    sw.current_tick = 0;
    for slot in &mut sw.slots {
        slot.head  = ptr::null_mut();
        slot.count = 0;
    }
}

pub fn timer_current_tick() -> u32 {
    // SAFETY: `sleep_wheel_mut` points at the statically allocated `SLEEP_WHEEL`; this performs a
    // single aligned `u32` read of the scheduler-owned `current_tick` counter, whose only writer
    // is the timer tick path advancing the same word.
    unsafe { (*sleep_wheel_mut()).current_tick }
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct` whose `proc` is null or a live `ProcMeta`; the
/// caller must be running on the timer/scheduler path (the wheel is not otherwise locked).
pub unsafe fn timer_wheel_add(task: *mut TaskStruct, sleep_ticks: u32) {
    if task.is_null() {
        return;
    }
    // SAFETY: `sleep_wheel_mut` points at the statically allocated `SLEEP_WHEEL`, which only the
    // timer/scheduler path mutates (see # Safety).
    let sw = unsafe { &mut *sleep_wheel_mut() };
    let wake_tick = sw.current_tick.wrapping_add(sleep_ticks);
    // SAFETY: `task` is non-null and live (see # Safety), so reading its `proc` field is in
    // bounds.
    let p = unsafe { (*task).proc };
    if !p.is_null() {
        // SAFETY: `p` is the task's live `ProcMeta` (non-null checked here).
        unsafe { (*p).sleep_until = wake_tick };
    }
    let slot_idx = (wake_tick as usize) % WHEEL_SIZE;
    sw.slots[slot_idx].push(task);
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct` whose `proc` is null or a live `ProcMeta`; the
/// caller must be running on the timer/scheduler path (the wheel is not otherwise locked).
pub unsafe fn timer_wheel_remove(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    // SAFETY: `sleep_wheel_mut` points at the statically allocated `SLEEP_WHEEL`, whose intrusive
    // lists are only mutated from the timer/scheduler path (see # Safety).
    let sw = unsafe { &mut *sleep_wheel_mut() };
    // SAFETY: `task` is live (see # Safety), so reading its `proc` field is in bounds.
    let task_p = unsafe { (*task).proc };
    // SAFETY: `task_p` is the task's live `ProcMeta` (the wheel only ever holds such tasks).
    let sleep_until = unsafe { (*task_p).sleep_until };
    let slot_idx = (sleep_until as usize) % WHEEL_SIZE;
    let slot = &mut sw.slots[slot_idx];

    let mut prev: *mut TaskStruct = ptr::null_mut();
    let mut cur = slot.head;

    while !cur.is_null() {
        if cur == task {
            // SAFETY: `task_p` is live, so reading its `wait_next` is in bounds.
            let task_next = unsafe { (*task_p).wait_next };
            if prev.is_null() {
                slot.head = task_next;
            } else {
                // SAFETY: `prev` was reached by walking this slot's chain, so it is a live task.
                let prev_p = unsafe { (*prev).proc };
                // SAFETY: `prev_p` is that live task's `ProcMeta`.
                unsafe { (*prev_p).wait_next = task_next };
            }
            slot.count -= 1;
            // SAFETY: `task_p` is live.
            unsafe { (*task_p).wait_next = ptr::null_mut() };
            return;
        }
        prev = cur;
        // SAFETY: `cur` was reached by walking this slot's chain, so it is a live task.
        let cur_p = unsafe { (*cur).proc };
        cur = if cur_p.is_null() {
            ptr::null_mut()
        } else {
            // SAFETY: `cur_p` is that live task's `ProcMeta`.
            unsafe { (*cur_p).wait_next }
        };
    }
}

pub fn timer_wheel_tick() {
    // SAFETY: `sleep_wheel_mut` points at the statically allocated `SLEEP_WHEEL`; the timer path
    // that calls this is running alone for this wheel, and all list mutation below happens under
    // `SCHEDULER_LOCK`.
    let sw = unsafe { &mut *sleep_wheel_mut() };
    sw.current_tick = sw.current_tick.wrapping_add(1);
    let now = sw.current_tick;
    let slot_idx = (now as usize) % WHEEL_SIZE;
    let mut cur = sw.slots[slot_idx].drain();

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the timer path holds no lock
    // when it calls in.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    while !cur.is_null() {
        // SAFETY: `cur` was placed on this slot by this module, so it is a live task.
        let cur_p = unsafe { (*cur).proc };
        // SAFETY: `cur_p` is that live task's `ProcMeta`.
        let next = unsafe { (*cur_p).wait_next };
        // SAFETY: `cur_p` is live.
        unsafe { (*cur_p).wait_next = ptr::null_mut() };
        // SAFETY: `cur_p` is live.
        let sleep_until = unsafe { (*cur_p).sleep_until };
        // SAFETY: `cur` is live.
        let state = unsafe { (*cur).state };

        if sleep_until <= now {
            if matches!(state, TaskState::Sleeping) {
                // SAFETY: `cur_p` is live.
                unsafe { (*cur_p).sleep_until = 0 };
                // SAFETY: `cur` is live and owned by this drained wheel list.
                unsafe { (*cur).state = TaskState::Ready };
                // SAFETY: `cur` is live.
                let priority = unsafe { (*cur).priority };
                // SAFETY: `cur` is live; the enqueue runs under `SCHEDULER_LOCK`.
                unsafe { mlfq::mlfq_enqueue_locked(cur, priority) };
            }
        } else {
            let future_slot = (sleep_until as usize) % WHEEL_SIZE;
            sw.slots[future_slot].push(cur);
        }

        cur = next;
    }

    // SAFETY: the lock was acquired above and has not been released yet.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
}

/// # Safety
///
/// Must be called from a task context (a current task exists) with `SCHEDULER_LOCK` free.
#[no_mangle]
pub unsafe extern "C" fn sched_sleep_ticks(ticks: u32) {
    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the caller must not hold it
    // (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `current_task` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let cur = unsafe { crate::task::current_task };
    // SAFETY: `cur` is the live current task (non-null checked just below).
    let cur_p = if cur.is_null() { ptr::null_mut() } else { unsafe { (*cur).proc } };
    if cur.is_null() || cur_p.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return;
    }

    // SAFETY: `sleep_wheel_mut` is the statically allocated `SLEEP_WHEEL`, mutated here under
    // `SCHEDULER_LOCK`.
    let sw = unsafe { &mut *sleep_wheel_mut() };
    let sleep_until = sw.current_tick.wrapping_add(ticks);
    // SAFETY: `cur` is the live current task (non-null checked above).
    unsafe { (*cur).state = TaskState::Sleeping };
    // SAFETY: `cur_p` is the live current task's `ProcMeta` (non-null checked above).
    unsafe { (*cur_p).sleep_until = sleep_until };
    let future_slot = (sleep_until as usize) % WHEEL_SIZE;
    sw.slots[future_slot].push(cur);

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `schedule` is the scheduler's core switch routine, called here as the documented
    // sleep path under no held lock.
    unsafe { crate::mlfq::schedule() };
}
