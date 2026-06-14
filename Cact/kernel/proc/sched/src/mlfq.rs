//! Multi-level feedback queue: per-priority ready queues, voluntary sleep queue,
//! periodic priority boost, and the main `schedule` / `on_timer_tick` entry points.
//!
//! All `TaskStruct` list mutation for MLFQ state is performed while holding
//! [`crate::task::SCHEDULER_LOCK`] (or during init before concurrency).

use core::cell::SyncUnsafeCell;
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

#[derive(Copy, Clone)]
struct MlfqQueue {
    head:  *mut TaskStruct,
    tail:  *mut TaskStruct,
    count: u32,
}

impl MlfqQueue {
    const fn empty() -> Self {
        Self { head: ptr::null_mut(), tail: ptr::null_mut(), count: 0 }
    }

    fn push(&mut self, task: &mut TaskStruct) {
        let tp: *mut TaskStruct = task;
        task.queue_next = ptr::null_mut();
        if self.tail.is_null() {
            self.head = tp;
            self.tail = tp;
        } else {
            unsafe {
                (*self.tail).queue_next = tp;
            }
            self.tail = tp;
        }
        self.count += 1;
    }

    fn pop(&mut self) -> *mut TaskStruct {
        if self.head.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            let t = self.head;
            self.head = (*t).queue_next;
            if self.head.is_null() {
                self.tail = ptr::null_mut();
            }
            (*t).queue_next = ptr::null_mut();
            self.count -= 1;
            t
        }
    }

    fn remove(&mut self, task: *mut TaskStruct) {
        if task.is_null() {
            return;
        }
        unsafe {
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
}

struct MlfqState {
    queues:        [MlfqQueue; MLFQ_LEVELS],
    sleep_queue:   MlfqQueue,
    boost_counter: u32,
}

/// Global MLFQ queues; `TaskStruct` pointers form intrusive lists. Access is serialized
/// with the scheduler IRQ lock except during `mlfq_init`.
unsafe impl Sync for MlfqState {}

impl MlfqState {
    const fn new() -> Self {
        Self {
            queues: [
                MlfqQueue::empty(),
                MlfqQueue::empty(),
                MlfqQueue::empty(),
                MlfqQueue::empty(),
            ],
            sleep_queue:   MlfqQueue::empty(),
            boost_counter: 0,
        }
    }
}

static MLFQ_STATE: SyncUnsafeCell<MlfqState> = SyncUnsafeCell::new(MlfqState::new());

static SCHEDULE_IN_PROGRESS: AtomicU32 = AtomicU32::new(0);
static REAP_COUNTER: AtomicU32 = AtomicU32::new(0);

#[inline]
fn mlfq_state_mut() -> *mut MlfqState {
    MLFQ_STATE.get()
}

pub fn mlfq_init() {
    unsafe {
        let s = &mut *mlfq_state_mut();
        for q in &mut s.queues {
            q.head  = ptr::null_mut();
            q.tail  = ptr::null_mut();
            q.count = 0;
        }
        s.sleep_queue = MlfqQueue::empty();
        s.boost_counter = 0;
    }
}

pub fn mlfq_enqueue_locked(task: *mut TaskStruct, level: u32) {
    if task.is_null() {
        return;
    }
    unsafe {
        let s = &mut *mlfq_state_mut();
        let level = level.min(MLFQ_LEVELS as u32 - 1) as usize;
        let t = &mut *task;
        t.priority = level as u32;
        s.queues[level].push(t);
    }
}

#[no_mangle]
pub unsafe extern "C" fn sched_mlfq_enqueue_locked(task: *mut TaskStruct, level: u32) {
    mlfq_enqueue_locked(task, level);
}

pub fn mlfq_sleep_locked(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    unsafe {
        let s = &mut *mlfq_state_mut();
        s.sleep_queue.push(&mut *task);
    }
}

pub fn mlfq_remove_from_sleep(task: *mut TaskStruct) {
    unsafe {
        let s = &mut *mlfq_state_mut();
        s.sleep_queue.remove(task);
    }
}

pub fn mlfq_remove(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    unsafe {
        let s = &mut *mlfq_state_mut();
        let level = (*task).priority.min(MLFQ_LEVELS as u32 - 1) as usize;
        s.queues[level].remove(task);
    }
}

fn pick_next_task() -> *mut TaskStruct {
    unsafe {
        let s = &mut *mlfq_state_mut();
        for i in 0..MLFQ_LEVELS {
            let t = s.queues[i].pop();
            if !t.is_null() {
                return t;
            }
        }
        ptr::null_mut()
    }
}

fn do_priority_boost() {
    unsafe {
        let s = &mut *mlfq_state_mut();
        for level in 2..MLFQ_LEVELS {
            loop {
                let t = s.queues[level].pop();
                if t.is_null() {
                    break;
                }
                (*t).ticks_used = 0;
                (*t).priority = BOOST_TARGET;
                s.queues[BOOST_TARGET as usize].push(&mut *t);
            }
        }
        let cur = crate::task::current_task;
        if !cur.is_null() && (*cur).priority > BOOST_TARGET && (*cur).priority != MLFQ_LEVEL_RT {
            (*cur).priority   = BOOST_TARGET;
            (*cur).ticks_used = 0;
        }
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
        let parent_pid = (*(*prev).proc).parent_pid;
        if parent_pid != 0 {
            crate::task::task_signal_locked(parent_pid, crate::task::SIGCHLD);
            let parent = crate::task::find_task_by_pid(parent_pid);
            if !parent.is_null() && matches!((*parent).state, TaskState::Waiting) {
                (*parent).state = TaskState::Ready;
                mlfq_enqueue_locked(parent, (*parent).priority);
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
        TaskState::Waiting | TaskState::Zombie => {}
        TaskState::Ready => {}
    }

    (*next).state = TaskState::Running;
    crate::task::current_task = next;

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    let prev_pd = (*prev).page_directory;
    let next_pd = (*next).page_directory;

    let effective_next_pd = if next_pd.is_null() {
        ffi::page_directory.get()
    } else {
        next_pd
    };

    let effective_prev_pd = if prev_pd.is_null() {
        ffi::page_directory.get()
    } else {
        prev_pd
    };

    ffi::vmm_sync_kernel_mmio_mappings(effective_next_pd);

    if effective_next_pd != effective_prev_pd {
        ffi::switch_paging(effective_next_pd);
    }

    if !(*next).proc.is_null() && !(*(*next).proc).stack_base.is_null() {
        unsafe { (*ffi::tss_entry.get()).esp0 = (*(*next).proc).stack_base as u32 + crate::task::KERNEL_STACK_SIZE as u32; }
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

fn wake_expired_sleepers() {
    let now = crate::timer_wheel::timer_current_tick();
    unsafe {
        let s = &mut *mlfq_state_mut();
        let sq = &mut s.sleep_queue;
        let mut prev: *mut TaskStruct = ptr::null_mut();
        let mut cur = sq.head;

        while !cur.is_null() {
            let next = (*cur).queue_next;
            if (*(*cur).proc).sleep_until != 0 && now >= (*(*cur).proc).sleep_until {
                if prev.is_null() {
                    sq.head = next;
                } else {
                    (*prev).queue_next = next;
                }
                if sq.tail == cur {
                    sq.tail = prev;
                }
                sq.count -= 1;

                (*(*cur).proc).sleep_until = 0;
                (*cur).state = TaskState::Ready;
                mlfq_enqueue_locked(cur, (*cur).priority);
            } else {
                prev = cur;
            }
            cur = next;
        }
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

    {
        let s = &mut *mlfq_state_mut();
        s.boost_counter = s.boost_counter.saturating_add(1);
        if s.boost_counter >= BOOST_INTERVAL {
            s.boost_counter = 0;
            do_priority_boost();
        }
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
    if task.is_null() {
        return;
    }
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    match (*task).state {
        TaskState::Sleeping => {
            mlfq_remove_from_sleep(task);
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

pub fn task_voluntary_block(task: *mut TaskStruct, new_state: TaskState) {
    if task.is_null() {
        return;
    }
    unsafe {
        let t = &mut *task;
        let quantum = MLFQ_QUANTUM[t.priority.min(MLFQ_LEVELS as u32 - 1) as usize];
        if t.ticks_used < quantum / 2 + 1 && t.priority > MLFQ_LEVEL_INTERACTIVE {
            t.priority -= 1;
        }
        t.ticks_used = 0;
        t.state = new_state;
        if matches!(new_state, TaskState::Sleeping) {
            mlfq_sleep_locked(task);
        }
    }
}
