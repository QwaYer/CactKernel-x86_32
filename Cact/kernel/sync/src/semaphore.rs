use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::hal;
use crate::mutex::MUTEX_WAIT_QUEUE_MAX;
use crate::sched_link;
use crate::task_abi::{TaskState, TaskStruct};

#[repr(C)]
pub struct semaphore_t {
    pub guard:        crate::spinlock::spinlock_t,
    pub count:        AtomicI32,
    pub waiters:      [*mut TaskStruct; MUTEX_WAIT_QUEUE_MAX],
    pub waiter_count: u32,
}

unsafe impl Send for semaphore_t {}
unsafe impl Sync for semaphore_t {}

#[no_mangle]
pub unsafe extern "C" fn sema_init(s: *mut semaphore_t, val: i32) {
    sema_init_impl(&mut *s, val);
}

fn sema_init_impl(s: &mut semaphore_t, val: i32) {
    s.guard.init();
    s.count.store(val, Ordering::Relaxed);
    s.waiter_count = 0;
    for slot in &mut s.waiters {
        *slot = ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn sema_down(s: *mut semaphore_t) {
    sema_down_impl(&mut *s);
}

fn sema_down_impl(s: &mut semaphore_t) {
    loop {
        let cur_val = s.count.load(Ordering::Acquire);
        if cur_val > 0 {
            if s.count
                .compare_exchange(cur_val, cur_val - 1, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
            {
                return;
            }
            continue;
        }

        let sched = sched_link::scheduler_lock_mut();
        sched.acquire();
        s.guard.acquire();

        if s.count.load(Ordering::Relaxed) > 0 {
            s.guard.release();
            sched.release();
            continue;
        }

        let cur = sched_link::current_task_ptr();
        if !cur.is_null() && (s.waiter_count as usize) < MUTEX_WAIT_QUEUE_MAX {
            let idx = s.waiter_count as usize;
            s.waiters[idx] = cur;
            s.waiter_count += 1;

            sched_link::task_state_set(cur, TaskState::Sleeping);

            s.guard.release();
            sched.release();

            sched_link::schedule_yield();
        } else {
            s.guard.release();
            sched.release();
            hal::pause_cpu();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn sema_up(s: *mut semaphore_t) {
    sema_up_impl(&mut *s);
}

fn sema_up_impl(s: &mut semaphore_t) {
    let sched = sched_link::scheduler_lock_mut();
    sched.acquire();
    s.guard.acquire();

    if s.waiter_count > 0 {
        let woken = s.waiters[0];
        let count = s.waiter_count as usize;
        for i in 1..count {
            s.waiters[i - 1] = s.waiters[i];
        }
        s.waiter_count -= 1;

        s.guard.release();

        if !woken.is_null() && sched_link::task_state_get(woken) == TaskState::Sleeping {
            sched_link::task_state_set(woken, TaskState::Ready);
            sched_link::mlfq_enqueue(woken, sched_link::task_priority_get(woken));
        }
    } else {
        s.count.fetch_add(1, Ordering::Release);
        s.guard.release();
    }

    sched.release();
}
