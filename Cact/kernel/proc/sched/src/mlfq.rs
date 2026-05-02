use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};
use crate::task::{TaskStruct, TaskState, SCHEDULER_LOCK};
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::ffi;

pub const MLFQ_LEVELS: usize = 4;

pub const MLFQ_LEVEL_RT:          u32 = 0;
pub const MLFQ_LEVEL_INTERACTIVE: u32 = 1;
pub const MLFQ_LEVEL_NORMAL:      u32 = 2;
pub const MLFQ_LEVEL_BACKGROUND:  u32 = 3;

pub const MLFQ_QUANTUM: [u32; MLFQ_LEVELS] = [5, 1, 2, 4];

const BOOST_INTERVAL: u32 = 50;

const BOOST_TARGET: u32 = MLFQ_LEVEL_INTERACTIVE;


struct MlfqQueue {
    head:  *mut TaskStruct,
    tail:  *mut TaskStruct,
    count: u32,
}

impl MlfqQueue {
    const fn empty() -> Self {
        Self { head: ptr::null_mut(), tail: ptr::null_mut(), count: 0 }
    }

    unsafe fn push(&mut self, task: *mut TaskStruct) {
        (*task).queue_next = ptr::null_mut();
        if self.tail.is_null() {
            self.head = task;
            self.tail = task;
        } else {
            (*self.tail).queue_next = task;
            self.tail = task;
        }
        self.count += 1;
    }

    unsafe fn pop(&mut self) -> *mut TaskStruct {
        if self.head.is_null() { return ptr::null_mut(); }
        let t = self.head;
        self.head = (*t).queue_next;
        if self.head.is_null() { self.tail = ptr::null_mut(); }
        (*t).queue_next = ptr::null_mut();
        self.count -= 1;
        t
    }

    unsafe fn remove(&mut self, task: *mut TaskStruct) {
        let mut prev: *mut TaskStruct = ptr::null_mut();
        let mut cur = self.head;
        while !cur.is_null() {
            if cur == task {
                if prev.is_null() {
                    self.head = (*task).queue_next;
                } else {
                    (*prev).queue_next = (*task).queue_next;
                }
                if self.tail == task {
                    self.tail = prev;
                }
                self.count -= 1;
                (*task).queue_next = ptr::null_mut();
                return;
            }
            prev = cur;
            cur  = (*cur).queue_next;
        }
    }
}

unsafe impl Send for MlfqQueue {}
unsafe impl Sync for MlfqQueue {}


static mut QUEUES: [MlfqQueue; MLFQ_LEVELS] = [
    MlfqQueue::empty(),
    MlfqQueue::empty(),
    MlfqQueue::empty(),
    MlfqQueue::empty(),
];

static mut SLEEP_QUEUE: MlfqQueue = MlfqQueue::empty();

static mut BOOST_COUNTER: u32 = 0;

static SCHEDULE_IN_PROGRESS: AtomicU32 = AtomicU32::new(0);
static REAP_COUNTER: AtomicU32 = AtomicU32::new(0);


//Public api
pub unsafe fn mlfq_init() {
    for i in 0..MLFQ_LEVELS {
        let q = ptr::addr_of_mut!(QUEUES[i]);
        (*q).head  = ptr::null_mut();
        (*q).tail  = ptr::null_mut();
        (*q).count = 0;
    }
    ptr::addr_of_mut!(SLEEP_QUEUE).write(MlfqQueue::empty());
    BOOST_COUNTER = 0;
}

pub unsafe fn mlfq_enqueue_locked(task: *mut TaskStruct, level: u32) {
    let level = level.min(MLFQ_LEVELS as u32 - 1) as usize;
    (*task).priority = level as u32;
    (*ptr::addr_of_mut!(QUEUES[level])).push(task);
}

#[no_mangle]
pub unsafe extern "C" fn sched_mlfq_enqueue_locked(task: *mut TaskStruct, level: u32) {
    mlfq_enqueue_locked(task, level);
}

pub unsafe fn mlfq_sleep_locked(task: *mut TaskStruct) {
    (*ptr::addr_of_mut!(SLEEP_QUEUE)).push(task);
}

pub unsafe fn mlfq_remove_from_sleep(task: *mut TaskStruct) {
    (*ptr::addr_of_mut!(SLEEP_QUEUE)).remove(task);
}

pub unsafe fn mlfq_remove(task: *mut TaskStruct) {
    let level = (*task).priority.min(MLFQ_LEVELS as u32 - 1) as usize;
    (*ptr::addr_of_mut!(QUEUES[level])).remove(task);
}

unsafe fn pick_next_task() -> *mut TaskStruct {
    for i in 0..MLFQ_LEVELS {
        let t = (*ptr::addr_of_mut!(QUEUES[i])).pop();
        if !t.is_null() { return t; }
    }
    ptr::null_mut()
}

unsafe fn do_priority_boost() {
    for level in 2..MLFQ_LEVELS {
        loop {
            let t = (*ptr::addr_of_mut!(QUEUES[level])).pop();
            if t.is_null() { break; }
            (*t).ticks_used = 0;
            (*ptr::addr_of_mut!(QUEUES[BOOST_TARGET as usize])).push(t);
            (*t).priority = BOOST_TARGET;
        }
    }
    let cur = crate::task::current_task;
    if !cur.is_null() && (*cur).priority > BOOST_TARGET && (*cur).priority != MLFQ_LEVEL_RT {
        (*cur).priority  = BOOST_TARGET;
        (*cur).ticks_used = 0;
    }
}


#[no_mangle]
pub unsafe extern "C" fn schedule() {
    if SCHEDULE_IN_PROGRESS
        .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        return;
    }

    // Periodically reap zombie tasks whose parent is gone or already called waitpid().
    // Called here before acquiring SCHEDULER_LOCK so task_reap() can acquire it freely.
    if REAP_COUNTER.fetch_add(1, Ordering::Relaxed) % 128 == 127 {
        crate::task::task_reap();
    }

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    let prev = crate::task::current_task;
    if prev.is_null() || crate::task::task_list_head.is_null() {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        SCHEDULE_IN_PROGRESS.store(0, Ordering::Release);
        return;
    }

    if matches!((*prev).state, TaskState::Zombie) {
        let parent_pid = (*prev).parent_pid;
        if parent_pid != 0 {
            let mut buf = [0u8; 12];
            crate::ffi::kprint(b"[SCHED] zombie pid=\0".as_ptr());
            crate::ffi::itoa((*prev).pid as i32, buf.as_mut_ptr());
            crate::ffi::kprint(buf.as_ptr());
            crate::ffi::kprint(b" parent=\0".as_ptr());
            crate::ffi::itoa(parent_pid as i32, buf.as_mut_ptr());
            crate::ffi::kprint(buf.as_ptr());

            crate::task::task_signal_locked(parent_pid, crate::task::SIGCHLD);
            let parent = crate::task::find_task_by_pid(parent_pid);
            if !parent.is_null() && matches!((*parent).state, TaskState::Waiting) {
                (*parent).state = TaskState::Ready;
                mlfq_enqueue_locked(parent, (*parent).priority);
                crate::ffi::kprint(b" WOKE\n\0".as_ptr());
            } else {
                crate::ffi::kprint(b" parent_state=\0".as_ptr());
                if parent.is_null() {
                    crate::ffi::kprint(b"NOT_FOUND\n\0".as_ptr());
                } else {
                    crate::ffi::itoa((*parent).state as i32, buf.as_mut_ptr());
                    crate::ffi::kprint(buf.as_ptr());
                    crate::ffi::kprint(b"\n\0".as_ptr());
                }
            }
        }
    }

    wake_expired_sleepers();

    let next = pick_next_task();

    if next.is_null() || next == prev {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        SCHEDULE_IN_PROGRESS.store(0, Ordering::Release);
        return;
    }

    match (*prev).state {
        TaskState::Running => {
            (*prev).state = TaskState::Ready;
            mlfq_enqueue_locked(prev, (*prev).priority);
        }
        TaskState::Sleeping => {
            mlfq_sleep_locked(prev);
        }
        TaskState::Waiting | TaskState::Zombie => {
        }
        TaskState::Ready => {
        }
    }

    (*next).state = TaskState::Running;
    crate::task::current_task = next;

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    let prev_pd = (*prev).page_directory;
    let next_pd = (*next).page_directory;

    let effective_next_pd = if next_pd.is_null() {
        ptr::addr_of!(ffi::page_directory) as *mut u32
    } else {
        next_pd
    };

    let effective_prev_pd = if prev_pd.is_null() {
        ptr::addr_of!(ffi::page_directory) as *mut u32
    } else {
        prev_pd
    };

    if effective_next_pd != effective_prev_pd {
        ffi::switch_paging(effective_next_pd);
    }

    if !(*next).stack_base.is_null() {
        ffi::tss_entry.esp0 = (*next).stack_base as u32 + crate::task::KERNEL_STACK_SIZE as u32;
    }

    ffi::cli();
    SCHEDULE_IN_PROGRESS.store(0, Ordering::Release);
    ffi::switch_to(&raw mut (*prev).esp, (*next).esp);
    ffi::sti();

    let cur = crate::task::current_task;
    if !cur.is_null() {
        crate::task::task_handle_signals(cur);
    }
}

unsafe fn wake_expired_sleepers() {
    let now = crate::timer_wheel::timer_current_tick();
    let sq = ptr::addr_of_mut!(SLEEP_QUEUE);
    let mut prev: *mut TaskStruct = ptr::null_mut();
    let mut cur = (*sq).head;

    while !cur.is_null() {
        let next = (*cur).queue_next;
        if (*cur).sleep_until != 0 && now >= (*cur).sleep_until {
            if prev.is_null() {
                (*sq).head = next;
            } else {
                (*prev).queue_next = next;
            }
            if (*sq).tail == cur {
                (*sq).tail = prev;
            }
            (*sq).count -= 1;

            (*cur).sleep_until = 0;
            (*cur).state = TaskState::Ready;
            mlfq_enqueue_locked(cur, (*cur).priority);
        } else {
            prev = cur;
        }
        cur = next;
    }
}

#[no_mangle]
pub unsafe extern "C" fn on_timer_tick() {
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    let cur = crate::task::current_task;
    if cur.is_null() {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return;
    }

    (*cur).ticks_used += 1;
    let quantum = MLFQ_QUANTUM[(*cur).priority.min(MLFQ_LEVELS as u32 - 1) as usize];

    let need_preempt = (*cur).ticks_used >= quantum;

    if need_preempt {
        if (*cur).priority != MLFQ_LEVEL_RT && (*cur).priority < MLFQ_LEVEL_BACKGROUND {
            (*cur).priority += 1;
        }
        (*cur).ticks_used = 0;
    }

    BOOST_COUNTER += 1;
    if BOOST_COUNTER >= BOOST_INTERVAL {
        BOOST_COUNTER = 0;
        do_priority_boost();
    }

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    crate::task::task_check_timers();

    let live = crate::task::current_task;
    if !live.is_null() {
        crate::task::task_handle_signals(live);
    }

    if need_preempt {
        schedule();
    }
}

#[no_mangle]
pub unsafe extern "C" fn mlfq_wake_task(task: *mut TaskStruct) {
    if task.is_null() { return; }
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    match (*task).state {
        TaskState::Sleeping => {
            (*ptr::addr_of_mut!(SLEEP_QUEUE)).remove(task);
            (*task).state = TaskState::Ready;
            mlfq_enqueue_locked(task, (*task).priority);
        }
        TaskState::Waiting => {
            (*task).state = TaskState::Ready;
            mlfq_enqueue_locked(task, (*task).priority);
        }
        _ => {} 
    }
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);
}

pub unsafe fn task_voluntary_block(task: *mut TaskStruct, new_state: TaskState) {
    let quantum = MLFQ_QUANTUM[(*task).priority.min(MLFQ_LEVELS as u32 - 1) as usize];
    if (*task).ticks_used < quantum / 2 + 1 && (*task).priority > MLFQ_LEVEL_INTERACTIVE {
        (*task).priority -= 1;
    }
    (*task).ticks_used = 0;
    (*task).state = new_state;
    if matches!(new_state, TaskState::Sleeping) {
        mlfq_sleep_locked(task);
    }
}