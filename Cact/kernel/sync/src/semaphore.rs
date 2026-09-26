//! Counting semaphore with a bounded FIFO waiter list; uses the scheduler when `down`
//! must block on a zero count.

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

// SAFETY: `semaphore_t` is a plain data record whose fields are only mutated
// while its internal `guard` spinlock is held (or during single-threaded
// `init`), so moving one between CPUs (Send) cannot expose a partial update.
unsafe impl Send for semaphore_t {}
// SAFETY: shared references to `semaphore_t` are safe because every read and
// write of `count`/`waiters`/`waiter_count` happens under `guard` (taken with
// the scheduler lock when a wake-up must be ordered); the raw task pointers are
// never dereferenced while not holding `guard`.
unsafe impl Sync for semaphore_t {}

/// # Safety
///
/// `s` must be non-null and properly aligned, and must point to storage the
/// caller owns exclusively for this call; it becomes an initialised semaphore
/// with `val` tokens.
#[no_mangle]
pub unsafe extern "C" fn sema_init(s: *mut semaphore_t, val: i32) {
    // SAFETY: the caller contract (see # Safety) makes `s` a valid, aligned,
    // unaliased pointer, so creating the unique `&mut` is sound.
    sema_init_impl(unsafe { &mut *s }, val);
}

fn sema_init_impl(s: &mut semaphore_t, val: i32) {
    s.guard.init();
    s.count.store(val, Ordering::Relaxed);
    s.waiter_count = 0;
    s.waiters.fill(ptr::null_mut());
}

/// # Safety
///
/// `s` must be non-null and properly aligned, must point to an initialised
/// `semaphore_t` that stays live for the duration of the call, and must not be
/// accessed concurrently except through this semaphore's own operations.
#[no_mangle]
pub unsafe extern "C" fn down(s: *mut semaphore_t) {
    // SAFETY: the caller contract (see # Safety) makes `s` a valid, aligned,
    // unaliased pointer for the duration of the call.
    sema_down_impl(unsafe { &mut *s });
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

/// # Safety
///
/// `s` must be non-null and properly aligned, must point to an initialised
/// `semaphore_t` that stays live for the duration of the call, and must not be
/// accessed concurrently except through this semaphore's own operations.
#[no_mangle]
pub unsafe extern "C" fn up(s: *mut semaphore_t) {
    // SAFETY: the caller contract (see # Safety) makes `s` a valid, aligned,
    // unaliased pointer for the duration of the call.
    sema_up_impl(unsafe { &mut *s });
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

        // Hand the token over: the woken task re-checks `count` in
        // `sema_down_impl` and must find it incremented, otherwise it blocks
        // again and the wake-up is lost (the caller waits forever).
        s.count.fetch_add(1, Ordering::Release);

        s.guard.release();

        // `mlfq_wake_locked` unlinks a Sleeping task from the sleep queue
        // before enqueuing it; a plain enqueue would leave it in both lists.
        sched_link::mlfq_wake_locked(woken);
    } else {
        s.count.fetch_add(1, Ordering::Release);
        s.guard.release();
    }

    sched.release();
}
