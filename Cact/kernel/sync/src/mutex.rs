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
    pub locked:          AtomicU32,
    pub guard:           crate::spinlock::spinlock_t,
    pub owner:           *mut TaskStruct,
    pub waiters:         [*mut TaskStruct; MUTEX_WAIT_QUEUE_MAX],
    pub waiter_count:    u32,
    pub recursion_count: u32,
}

// SAFETY: `mutex_t` is a plain data record whose fields are only mutated while
// its internal `guard` spinlock is held (or during single-threaded `init`), so
// moving one between CPUs (Send) cannot expose a partially updated record.
unsafe impl Send for mutex_t {}
// SAFETY: shared references to `mutex_t` are safe because every read and write
// of `locked`/`owner`/`waiters`/`waiter_count`/`recursion_count` happens under
// `guard`, which serialises concurrent CPUs; the raw task pointers are never
// dereferenced while not holding `guard`.
unsafe impl Sync for mutex_t {}

/// # Safety
///
/// `m` must be non-null and properly aligned, and must point to storage the
/// caller owns exclusively for this call; it becomes an initialised mutex.
#[no_mangle]
pub unsafe extern "C" fn mutex_init(m: *mut mutex_t) {
    // SAFETY: the caller contract (see # Safety) makes `m` a valid, aligned,
    // unaliased pointer, so creating the unique `&mut` is sound.
    mutex_init_impl(unsafe { &mut *m });
}

fn mutex_init_impl(m: &mut mutex_t) {
    m.locked.store(0, Ordering::Relaxed);
    m.guard.init();
    m.owner = ptr::null_mut();
    m.waiter_count = 0;
    m.recursion_count = 0;
    m.waiters.fill(ptr::null_mut());
}

/// # Safety
///
/// `m` must be non-null and properly aligned, must point to an initialised
/// `mutex_t` that stays live for the duration of the call, and must not be
/// accessed concurrently except through this mutex's own lock protocol.
#[no_mangle]
pub unsafe extern "C" fn mutex_lock(m: *mut mutex_t) {
    // SAFETY: the caller contract (see # Safety) makes `m` a valid, aligned,
    // unaliased pointer for the duration of the call.
    mutex_lock_impl(unsafe { &mut *m });
}

fn mutex_lock_impl(m: &mut mutex_t) {
    loop {
        m.guard.acquire();

        if m.owner == sched_link::current_task_ptr() {
            m.recursion_count += 1;
            m.guard.release();
            return;
        }

        if m.locked.load(Ordering::Relaxed) == 0 {
            m.locked.store(1, Ordering::Relaxed);
            m.owner = sched_link::current_task_ptr();
            m.recursion_count = 0;
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

/// # Safety
///
/// `m` must be non-null and properly aligned, must point to an initialised
/// `mutex_t` that stays live for the duration of the call, and must not be
/// accessed concurrently except through this mutex's own lock protocol.
#[no_mangle]
pub unsafe extern "C" fn mutex_trylock(m: *mut mutex_t) -> i32 {
    // SAFETY: the caller contract (see # Safety) makes `m` a valid, aligned,
    // unaliased pointer for the duration of the call.
    mutex_trylock_impl(unsafe { &mut *m })
}

fn mutex_trylock_impl(m: &mut mutex_t) -> i32 {
    m.guard.acquire();
    if m.owner == sched_link::current_task_ptr() {
        m.recursion_count += 1;
        m.guard.release();
        return 0;
    }
    if m.locked.load(Ordering::Relaxed) == 0 {
        m.locked.store(1, Ordering::Relaxed);
        m.owner = sched_link::current_task_ptr();
        m.recursion_count = 0;
        m.guard.release();
        return 0;
    }
    m.guard.release();
    -1
}

/// # Safety
///
/// `m` must be non-null and properly aligned, must point to an initialised
/// `mutex_t` that stays live for the duration of the call, and must not be
/// accessed concurrently except through this mutex's own lock protocol.
#[no_mangle]
pub unsafe extern "C" fn mutex_unlock(m: *mut mutex_t) {
    // SAFETY: the caller contract (see # Safety) makes `m` a valid, aligned,
    // unaliased pointer for the duration of the call.
    mutex_unlock_impl(unsafe { &mut *m });
}

fn mutex_unlock_impl(m: &mut mutex_t) {
    m.guard.acquire();

    if m.locked.load(Ordering::Relaxed) == 0 {
        m.guard.release();
        sched_link::kprint_str(c"mutex: unlock of unlocked mutex!\n".as_ptr() as *const u8);
        return;
    }

    if m.owner == sched_link::current_task_ptr() && m.recursion_count > 0 {
        m.recursion_count -= 1;
        m.guard.release();
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
        // State-aware wake: also unlinks a Sleeping task from the sleep queue.
        sched_link::mlfq_wake_locked(woken);
        sched.release();
    } else {
        m.locked.store(0, Ordering::Release);
        m.owner = ptr::null_mut();
        m.guard.release();
    }
}
