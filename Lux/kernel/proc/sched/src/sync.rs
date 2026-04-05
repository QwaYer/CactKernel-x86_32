use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use crate::ffi;
use crate::task::TaskStruct;
use crate::mlfq;

pub const MUTEX_WAIT_QUEUE_MAX: usize = 64;   

#[repr(C)]
pub struct spinlock_t {
    pub locked: AtomicU32,
}

impl spinlock_t {
    pub const fn new() -> Self {
        Self { locked: AtomicU32::new(0) }
    }
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_init(lock: *mut spinlock_t) {
    (*lock).locked.store(0, Ordering::Relaxed);
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_acquire(lock: *mut spinlock_t) {
    loop {
        if (*lock).locked
            .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            return;
        }
        while (*lock).locked.load(Ordering::Relaxed) != 0 {
            ffi::pause();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_release(lock: *mut spinlock_t) {
    (*lock).locked.store(0, Ordering::Release);
}

#[repr(C)]
pub struct irq_spinlock_t {
    pub spin:        spinlock_t,
    pub saved_flags: u32,
}

impl irq_spinlock_t {
    pub const fn new() -> Self {
        Self {
            spin:        spinlock_t::new(),
            saved_flags: 0,
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_init(lock: *mut irq_spinlock_t) {
    spinlock_init(&raw mut (*lock).spin);
    (*lock).saved_flags = 0;
}

#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_acquire(lock: *mut irq_spinlock_t) {
    let flags = ffi::read_eflags();
    ffi::cli();
    spinlock_acquire(&raw mut (*lock).spin);
    (*lock).saved_flags = flags;
}

#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_release(lock: *mut irq_spinlock_t) {
    let flags = (*lock).saved_flags;
    spinlock_release(&raw mut (*lock).spin);
    if flags & (1 << 9) != 0 {
        ffi::sti();
    }
}

#[repr(C)]
pub struct mutex_t {
    pub locked:       AtomicU32,
    pub guard:        spinlock_t,
    pub owner:        *mut TaskStruct,
    pub waiters:      [*mut TaskStruct; MUTEX_WAIT_QUEUE_MAX],
    pub waiter_count: u32,
}

unsafe impl Send for mutex_t {}
unsafe impl Sync for mutex_t {}

#[no_mangle]
pub unsafe extern "C" fn mutex_init(m: *mut mutex_t) {
    (*m).locked.store(0, Ordering::Relaxed);
    spinlock_init(&raw mut (*m).guard);
    (*m).owner        = core::ptr::null_mut();
    (*m).waiter_count = 0;
    for i in 0..MUTEX_WAIT_QUEUE_MAX {
        (*m).waiters[i] = core::ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_lock(m: *mut mutex_t) {
    loop {
        spinlock_acquire(&raw mut (*m).guard);

        if (*m).locked.load(Ordering::Relaxed) == 0 {
            (*m).locked.store(1, Ordering::Relaxed);
            (*m).owner = crate::task::current_task;
            spinlock_release(&raw mut (*m).guard);
            return;
        }

        let cur = crate::task::current_task;
        if !cur.is_null() && ((*m).waiter_count as usize) < MUTEX_WAIT_QUEUE_MAX {
            let idx = (*m).waiter_count as usize;
            (*m).waiters[idx] = cur;
            (*m).waiter_count += 1;

            irq_spinlock_acquire(&raw mut crate::task::SCHEDULER_LOCK);
            (*cur).state = crate::task::TaskState::Sleeping;
            irq_spinlock_release(&raw mut crate::task::SCHEDULER_LOCK);

            spinlock_release(&raw mut (*m).guard);

            crate::mlfq::schedule();
        } else {
            spinlock_release(&raw mut (*m).guard);
            ffi::pause();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_trylock(m: *mut mutex_t) -> i32 {
    spinlock_acquire(&raw mut (*m).guard);
    if (*m).locked.load(Ordering::Relaxed) == 0 {
        (*m).locked.store(1, Ordering::Relaxed);
        (*m).owner = crate::task::current_task;
        spinlock_release(&raw mut (*m).guard);
        return 0;  
    }
    spinlock_release(&raw mut (*m).guard);
    -1  
}

#[no_mangle]
pub unsafe extern "C" fn mutex_unlock(m: *mut mutex_t) {
    spinlock_acquire(&raw mut (*m).guard);

    if (*m).locked.load(Ordering::Relaxed) == 0 {
        spinlock_release(&raw mut (*m).guard);
        ffi::kprint(b"mutex: unlock of unlocked mutex!\n\0".as_ptr());
        return;
    }

    if (*m).waiter_count > 0 {
        let woken = (*m).waiters[0];
        let count = (*m).waiter_count as usize;
        for i in 1..count {
            (*m).waiters[i - 1] = (*m).waiters[i];
        }
        (*m).waiter_count -= 1;

        (*m).owner = woken;
        spinlock_release(&raw mut (*m).guard);

        irq_spinlock_acquire(&raw mut crate::task::SCHEDULER_LOCK);
        if !woken.is_null() && matches!((*woken).state, crate::task::TaskState::Sleeping) {
            (*woken).state = crate::task::TaskState::Ready;
            mlfq::mlfq_enqueue_locked(woken, (*woken).priority);  
        }
        irq_spinlock_release(&raw mut crate::task::SCHEDULER_LOCK);
    } else {
        (*m).locked.store(0, Ordering::Release);
        (*m).owner = core::ptr::null_mut();
        spinlock_release(&raw mut (*m).guard);
    }
}

#[repr(C)]
pub struct semaphore_t {
    pub guard:        spinlock_t,
    pub count:        AtomicI32,        
    pub waiters:      [*mut TaskStruct; MUTEX_WAIT_QUEUE_MAX],
    pub waiter_count: u32,
}

unsafe impl Send for semaphore_t {}
unsafe impl Sync for semaphore_t {}

#[no_mangle]
pub unsafe extern "C" fn sema_init(s: *mut semaphore_t, val: i32) {
    spinlock_init(&raw mut (*s).guard);
    (*s).count.store(val, Ordering::Relaxed);
    (*s).waiter_count = 0;
    for i in 0..MUTEX_WAIT_QUEUE_MAX {
        (*s).waiters[i] = core::ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn sema_down(s: *mut semaphore_t) {
    loop {
        let cur_val = (*s).count.load(Ordering::Acquire);
        if cur_val > 0 {
            if (*s).count.compare_exchange(
                cur_val, cur_val - 1,
                Ordering::Acquire,
                Ordering::Relaxed,
            ).is_ok() {
                return;  
            }
            continue;
        }

        spinlock_acquire(&raw mut (*s).guard);

        if (*s).count.load(Ordering::Relaxed) > 0 {
            spinlock_release(&raw mut (*s).guard);
            continue;
        }

        let cur = crate::task::current_task;
        if !cur.is_null() && ((*s).waiter_count as usize) < MUTEX_WAIT_QUEUE_MAX {
            let idx = (*s).waiter_count as usize;
            (*s).waiters[idx] = cur;
            (*s).waiter_count += 1;

            irq_spinlock_acquire(&raw mut crate::task::SCHEDULER_LOCK);
            (*cur).state = crate::task::TaskState::Sleeping;
            irq_spinlock_release(&raw mut crate::task::SCHEDULER_LOCK);

            spinlock_release(&raw mut (*s).guard);
            crate::mlfq::schedule();
        } else {
            spinlock_release(&raw mut (*s).guard);
            ffi::pause();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn sema_up(s: *mut semaphore_t) {
    irq_spinlock_acquire(&raw mut crate::task::SCHEDULER_LOCK);
    spinlock_acquire(&raw mut (*s).guard);

    if (*s).waiter_count > 0 {
        let woken = (*s).waiters[0];
        let count = (*s).waiter_count as usize;
        for i in 1..count {
            (*s).waiters[i - 1] = (*s).waiters[i];
        }
        (*s).waiter_count -= 1;

        spinlock_release(&raw mut (*s).guard);

        if !woken.is_null() && matches!((*woken).state, crate::task::TaskState::Sleeping) {
            (*woken).state = crate::task::TaskState::Ready;
            mlfq::mlfq_enqueue_locked(woken, (*woken).priority);  
        }
    } else {
        (*s).count.fetch_add(1, Ordering::Release);
        spinlock_release(&raw mut (*s).guard);
    }

    irq_spinlock_release(&raw mut crate::task::SCHEDULER_LOCK);
}
