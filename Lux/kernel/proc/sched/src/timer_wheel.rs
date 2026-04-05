use core::ptr;
use crate::task::{TaskStruct, TaskState, SCHEDULER_LOCK};
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::mlfq;

const WHEEL_SIZE: usize = 256;

struct TimerSlot {
    head:  *mut TaskStruct,  
    count: u32,
}

impl TimerSlot {
    #[allow(dead_code)]
    const fn empty() -> Self {
        Self { head: ptr::null_mut(), count: 0 }
    }

    unsafe fn push(&mut self, task: *mut TaskStruct) {
        (*task).wait_next = self.head;
        self.head = task;
        self.count += 1;
    }

    unsafe fn drain(&mut self) -> *mut TaskStruct {
        let head = self.head;
        self.head  = ptr::null_mut();
        self.count = 0;
        head
    }
}

unsafe impl Send for TimerSlot {}
unsafe impl Sync for TimerSlot {}

struct TimerWheel {
    slots:        [TimerSlot; WHEEL_SIZE],
    current_tick: u32,
}

impl TimerWheel {
    const fn new() -> Self {
        unsafe {
            core::mem::transmute([0u8; core::mem::size_of::<TimerWheel>()])
        }
    }
}

static mut SLEEP_WHEEL: TimerWheel = TimerWheel::new();


//Public api
pub unsafe fn timer_wheel_global_init() {
    let sw = ptr::addr_of_mut!(SLEEP_WHEEL);
    (*sw).current_tick = 0;
    for i in 0..WHEEL_SIZE {
        (*sw).slots[i].head  = ptr::null_mut();
        (*sw).slots[i].count = 0;
    }
}

pub unsafe fn timer_current_tick() -> u32 {
    (*ptr::addr_of!(SLEEP_WHEEL)).current_tick
}

pub unsafe fn timer_wheel_add(task: *mut TaskStruct, sleep_ticks: u32) {
    let sw = ptr::addr_of_mut!(SLEEP_WHEEL);
    let wake_tick = (*sw).current_tick.wrapping_add(sleep_ticks);
    (*task).sleep_until = wake_tick;

    let slot_idx = (wake_tick as usize) % WHEEL_SIZE;
    (*sw).slots[slot_idx].push(task);
}

pub unsafe fn timer_wheel_remove(task: *mut TaskStruct) {
    let sw = ptr::addr_of_mut!(SLEEP_WHEEL);
    let slot_idx = ((*task).sleep_until as usize) % WHEEL_SIZE;
    let slot = ptr::addr_of_mut!((*sw).slots[slot_idx]);

    let mut prev: *mut TaskStruct = ptr::null_mut();
    let mut cur = (*slot).head;

    while !cur.is_null() {
        if cur == task {
            if prev.is_null() {
                (*slot).head = (*task).wait_next;
            } else {
                (*prev).wait_next = (*task).wait_next;
            }
            (*slot).count -= 1;
            (*task).wait_next = ptr::null_mut();
            return;
        }
        prev = cur;
        cur  = (*cur).wait_next;
    }
}


pub unsafe fn timer_wheel_tick() {
    let sw = ptr::addr_of_mut!(SLEEP_WHEEL);
    (*sw).current_tick = (*sw).current_tick.wrapping_add(1);
    let now = (*sw).current_tick;
    let slot_idx = (now as usize) % WHEEL_SIZE;

    let slot = ptr::addr_of_mut!((*sw).slots[slot_idx]);
    let mut cur = (*slot).drain();

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    while !cur.is_null() {
        let next = (*cur).wait_next;
        (*cur).wait_next = ptr::null_mut();

        if (*cur).sleep_until <= now {
            if matches!((*cur).state, TaskState::Sleeping) {
                (*cur).sleep_until = 0;
                (*cur).state       = TaskState::Ready;
                mlfq::mlfq_enqueue_locked(cur, (*cur).priority);
            }
        } else {
            let future_slot = ((*cur).sleep_until as usize) % WHEEL_SIZE;
            (*ptr::addr_of_mut!((*sw).slots[future_slot])).push(cur);
        }

        cur = next;
    }

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);
}

#[no_mangle]
pub unsafe extern "C" fn sched_sleep_ticks(ticks: u32) {
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    let cur = crate::task::current_task;
    if cur.is_null() {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return;
    }

    let sw = ptr::addr_of_mut!(SLEEP_WHEEL);
    (*cur).state       = TaskState::Sleeping;
    (*cur).sleep_until = (*sw).current_tick.wrapping_add(ticks);

    let future_slot = ((*cur).sleep_until as usize) % WHEEL_SIZE;
    (*ptr::addr_of_mut!((*sw).slots[future_slot])).push(cur);

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    crate::mlfq::schedule();
}
