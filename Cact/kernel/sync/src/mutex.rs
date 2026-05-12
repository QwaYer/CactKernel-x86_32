//! Sleeping mutex: FIFO wait queue, [`crate::spinlock::spinlock_t`] for the wait list,
//! and scheduler integration when the lock is held (block current task, yield).

use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::hal;
use crate::sched_link;
use crate::task_abi::{TaskState, TaskStruct};

pub const MUTEX_WAIT_QUEUE_MAX: usize = 64;

#[repr(C)]
pub struct mutex_t {
    pub locked:       AtomicU32,
    pub guard:        crate::spinlock::spinlock_t,
    pub owner:        *mut TaskStruct,
    pub waiters:      [*mut TaskStruct; MUTEX_WAIT_QUEUE_MAX],
    pub waiter_count: u32,
}

unsafe impl Send for mutex_t {}
unsafe impl Sync for mutex_t {}

#[no_mangle]
pub unsafe extern "C" fn mutex_init(m: *mut mutex_t) {
    mutex_init_impl(&mut *m);
}

fn mutex_init_impl(m: &mut mutex_t) {
    m.locked.store(0, Ordering::Relaxed);
    m.guard.init();
    m.owner = ptr::null_mut();
    m.waiter_count = 0;
    for slot in &mut m.waiters {
        *slot = ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_lock(m: *mut mutex_t) {
    mutex_lock_impl(&mut *m);
}

fn mutex_lock_impl(m: &mut mutex_t) {
    loop {
        m.guard.acquire();

        if m.locked.load(Ordering::Relaxed) == 0 {
            m.locked.store(1, Ordering::Relaxed);
            m.owner = sched_link::current_task_ptr();
            m.guard.release();
            return;
        }

        let cur = sched_link::current_task_ptr();
        if !cur.is_null() && (m.waiter_count as usize) < MUTEX_WAIT_QUEUE_MAX {
            let idx = m.waiter_count as usize;
            m.waiters[idx] = cur;
            m.waiter_count += 1;

            let sched = sched_link::scheduler_lock_mut();
            sched.acquire();
            sched_link::task_state_set(cur, TaskState::Sleeping);
            sched.release();

            m.guard.release();

            sched_link::schedule_yield();
        } else {
            m.guard.release();
            hal::pause_cpu();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_trylock(m: *mut mutex_t) -> i32 {
    mutex_trylock_impl(&mut *m)
}

fn mutex_trylock_impl(m: &mut mutex_t) -> i32 {
    m.guard.acquire();
    if m.locked.load(Ordering::Relaxed) == 0 {
        m.locked.store(1, Ordering::Relaxed);
        m.owner = sched_link::current_task_ptr();
        m.guard.release();
        return 0;
    }
    m.guard.release();
    -1
}

#[no_mangle]
pub unsafe extern "C" fn mutex_unlock(m: *mut mutex_t) {
    mutex_unlock_impl(&mut *m);
}

fn mutex_unlock_impl(m: &mut mutex_t) {
    m.guard.acquire();

    if m.locked.load(Ordering::Relaxed) == 0 {
        m.guard.release();
        sched_link::kprint_str(b"mutex: unlock of unlocked mutex!\n\0".as_ptr());
        return;
    }

    if m.waiter_count > 0 {
        let woken = m.waiters[0];
        let count = m.waiter_count as usize;
        for i in 1..count {
            m.waiters[i - 1] = m.waiters[i];
        }
        m.waiter_count -= 1;

        m.owner = woken;
        m.guard.release();

        let sched = sched_link::scheduler_lock_mut();
        sched.acquire();
        if !woken.is_null() && sched_link::task_state_get(woken) == TaskState::Sleeping {
            sched_link::task_state_set(woken, TaskState::Ready);
            sched_link::mlfq_enqueue(woken, sched_link::task_priority_get(woken));
        }
        sched.release();
    } else {
        m.locked.store(0, Ordering::Release);
        m.owner = ptr::null_mut();
        m.guard.release();
    }
}
