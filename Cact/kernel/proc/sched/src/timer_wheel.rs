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
        unsafe {
            let p = (*task).proc;
            if !p.is_null() {
                (*p).wait_next = self.head;
            }
            self.head = task;
            self.count += 1;
        }
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
    let sw = unsafe { &mut *sleep_wheel_mut() };
    sw.current_tick = 0;
    for slot in &mut sw.slots {
        slot.head  = ptr::null_mut();
        slot.count = 0;
    }
}

pub fn timer_current_tick() -> u32 {
    unsafe { (*sleep_wheel_mut()).current_tick }
}

pub fn timer_wheel_add(task: *mut TaskStruct, sleep_ticks: u32) {
    if task.is_null() {
        return;
    }
    unsafe {
        let sw = &mut *sleep_wheel_mut();
        let wake_tick = sw.current_tick.wrapping_add(sleep_ticks);
        if !(*task).proc.is_null() {
            (*(*task).proc).sleep_until = wake_tick;
        }
        let slot_idx = (wake_tick as usize) % WHEEL_SIZE;
        sw.slots[slot_idx].push(task);
    }
}

pub fn timer_wheel_remove(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    unsafe {
        let sw = &mut *sleep_wheel_mut();
        let slot_idx = ((*(*task).proc).sleep_until as usize) % WHEEL_SIZE;
        let slot = &mut sw.slots[slot_idx];

        let mut prev: *mut TaskStruct = ptr::null_mut();
        let mut cur = slot.head;

        while !cur.is_null() {
            if cur == task {
                if prev.is_null() {
                    slot.head = (*(*task).proc).wait_next;
                } else {
                    (*(*prev).proc).wait_next = (*(*task).proc).wait_next;
                }
                slot.count -= 1;
                (*(*task).proc).wait_next = ptr::null_mut();
                return;
            }
            prev = cur;
            cur = if !(*cur).proc.is_null() { (*(*cur).proc).wait_next } else { ptr::null_mut() };
        }
    }
}

pub fn timer_wheel_tick() {
    unsafe {
        let sw = &mut *sleep_wheel_mut();
        sw.current_tick = sw.current_tick.wrapping_add(1);
        let now = sw.current_tick;
        let slot_idx = (now as usize) % WHEEL_SIZE;
        let mut cur = sw.slots[slot_idx].drain();

        irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

        while !cur.is_null() {
            let next = (*(*cur).proc).wait_next;
            (*(*cur).proc).wait_next = ptr::null_mut();

            if (*(*cur).proc).sleep_until <= now {
                if matches!((*cur).state, TaskState::Sleeping) {
                    (*(*cur).proc).sleep_until = 0;
                    (*cur).state       = TaskState::Ready;
                    mlfq::mlfq_enqueue_locked(cur, (*cur).priority);
                }
            } else {
                let future_slot = ((*(*cur).proc).sleep_until as usize) % WHEEL_SIZE;
                sw.slots[future_slot].push(&mut *cur);
            }

            cur = next;
        }

        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
    }
}

#[no_mangle]
pub unsafe extern "C" fn sched_sleep_ticks(ticks: u32) {
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    let cur = crate::task::current_task;
    if cur.is_null() || (*cur).proc.is_null() {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return;
    }

    unsafe {
        let sw = &mut *sleep_wheel_mut();
        let p = (*cur).proc;
        (*cur).state       = TaskState::Sleeping;
        (*p).sleep_until = sw.current_tick.wrapping_add(ticks);
        let future_slot = ((*p).sleep_until as usize) % WHEEL_SIZE;
        sw.slots[future_slot].push(&mut *cur);
    }

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    crate::mlfq::schedule();
}
