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
            // SAFETY: `self.tail` is non-null and, per this queue's invariant, points at the last
            // element of the intrusive list, a live task; the store only rewrites its
            // `queue_next` link.
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
        let t = self.head;
        // SAFETY: `t` is non-null (this queue's head) and is a live task still owned by this
        // queue, so reading its `queue_next` is in bounds.
        let next = unsafe { (*t).queue_next };
        self.head = next;
        if self.head.is_null() {
            self.tail = ptr::null_mut();
        }
        // SAFETY: `t` is that live task, so clearing its link is in bounds.
        unsafe { (*t).queue_next = ptr::null_mut() };
        self.count -= 1;
        t
    }

    fn remove(&mut self, task: *mut TaskStruct) {
        if task.is_null() {
            return;
        }
        let mut prev: *mut TaskStruct = ptr::null_mut();
        let mut cur = self.head;
        while !cur.is_null() {
            if cur == task {
                // SAFETY: `task` is non-null and owned by this queue, so reading its link is in
                // bounds.
                let task_next = unsafe { (*task).queue_next };
                if prev.is_null() {
                    self.head = task_next;
                } else {
                    // SAFETY: `prev` was reached by walking this queue's chain, so it is a live
                    // task still owned by the queue; the store only rewrites its link.
                    unsafe { (*prev).queue_next = task_next };
                }
                if self.tail == task {
                    self.tail = prev;
                }
                self.count -= 1;
                // SAFETY: `task` is live and owned by this queue, so clearing its link is in
                // bounds.
                unsafe { (*task).queue_next = ptr::null_mut() };
                return;
            }
            prev = cur;
            // SAFETY: `cur` was reached by walking this queue's chain, so it is a live task.
            cur = unsafe { (*cur).queue_next };
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
// SAFETY: `MlfqState` contains only raw `*mut TaskStruct` list heads/tails whose links are
// mutated under `SCHEDULER_LOCK` (or during single-threaded `mlfq_init`), so sharing the static
// across CPUs cannot produce an unsynchronised access.
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
    // SAFETY: called from `task_init` during single-threaded boot, before interrupts are enabled
    // or any other CPU exists, so `MLFQ_STATE` is exclusively reachable here.
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

/// Runnable task count summed over all MLFQ levels (the idle task is never
/// enqueued, so it is excluded).  Caller must hold the scheduler lock (or be
/// in a single-threaded/interrupt context).
pub(crate) fn mlfq_runnable_count() -> u32 {
    // SAFETY: `mlfq_state_mut` yields the statically allocated `MLFQ_STATE`; the queue counters
    // are only mutated under `SCHEDULER_LOCK` (or during single-threaded init), and this call only
    // reads them.
    unsafe {
        let s = &*mlfq_state_mut();
        s.queues.iter().map(|q| q.count).sum()
    }
}

/// Highest-priority (lowest-level) non-empty ready queue, if any.
pub(crate) fn mlfq_highest_runnable_level() -> Option<u32> {
    // SAFETY: `mlfq_state_mut` yields the statically allocated `MLFQ_STATE`; the queues are only
    // mutated under `SCHEDULER_LOCK` (or during single-threaded init) and this only reads their
    // counters.
    unsafe {
        let s = &*mlfq_state_mut();
        (0..MLFQ_LEVELS)
            .find(|&l| s.queues[l].count > 0)
            .map(|l| l as u32)
    }
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct`; the caller must hold `SCHEDULER_LOCK`.
pub unsafe fn mlfq_enqueue_locked(task: *mut TaskStruct, level: u32) {
    if task.is_null() {
        return;
    }
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; the caller holds `SCHEDULER_LOCK`
    // (see # Safety), so exclusive access is sound.
    let s = unsafe { &mut *mlfq_state_mut() };
    let lvl = level.min(MLFQ_LEVELS as u32 - 1) as usize;
    // SAFETY: `task` is non-null and live (see # Safety).
    let t = unsafe { &mut *task };
    t.priority = lvl as u32;
    s.queues[lvl].push(t);
    crate::mlfq_map::on_enqueue(level.min(MLFQ_LEVELS as u32 - 1));
}

/// `mlfq_enqueue_locked` for the C kernel: the caller must already hold `SCHEDULER_LOCK`.
///
/// # Safety
///
/// `task` must be null or a live `TaskStruct`, and the caller must hold `SCHEDULER_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn sched_mlfq_enqueue_locked(task: *mut TaskStruct, level: u32) {
    // SAFETY: forwards this function's contract (live `task`, lock held) to the Rust helper.
    unsafe { mlfq_enqueue_locked(task, level) };
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct`; the caller must hold `SCHEDULER_LOCK`.
pub unsafe fn mlfq_sleep_locked(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; the caller holds `SCHEDULER_LOCK`
    // (see # Safety).
    let s = unsafe { &mut *mlfq_state_mut() };
    // SAFETY: `task` is non-null and live (see # Safety).
    let t = unsafe { &mut *task };
    s.sleep_queue.push(t);
}

pub fn mlfq_remove_from_sleep(task: *mut TaskStruct) {
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; callers hold `SCHEDULER_LOCK`, so
    // the unlink from the sleep queue cannot race another CPU.
    unsafe {
        let s = &mut *mlfq_state_mut();
        s.sleep_queue.remove(task);
    }
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct`; the caller must hold `SCHEDULER_LOCK`.
pub unsafe fn mlfq_remove(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; the caller holds `SCHEDULER_LOCK`
    // (see # Safety).
    let s = unsafe { &mut *mlfq_state_mut() };
    // SAFETY: `task` is non-null and live (see # Safety).
    let priority = unsafe { (*task).priority };
    let lvl = priority.min(MLFQ_LEVELS as u32 - 1) as usize;
    s.queues[lvl].remove(task);
}

fn pick_next_task() -> *mut TaskStruct {
    // SAFETY: `mlfq_state_mut` yields the statically allocated `MLFQ_STATE`; called with
    // `SCHEDULER_LOCK` held, and each `pop` returns a live task owned by the queue it came from.
    unsafe {
        let s = &mut *mlfq_state_mut();
        for queue in &mut s.queues {
            let t = queue.pop();
            if !t.is_null() {
                return t;
            }
        }
        ptr::null_mut()
    }
}

fn do_priority_boost() {
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; called from `on_timer_tick` with
    // `SCHEDULER_LOCK` held.
    let s = unsafe { &mut *mlfq_state_mut() };
    for level in 2..MLFQ_LEVELS {
        loop {
            let t = s.queues[level].pop();
            if t.is_null() {
                break;
            }
            // SAFETY: `t` was just popped from this scheduler's own queue, so it is a live task.
            let t_ref = unsafe { &mut *t };
            t_ref.ticks_used = 0;
            t_ref.priority = BOOST_TARGET;
            s.queues[BOOST_TARGET as usize].push(t_ref);
        }
    }
    // SAFETY: `current_task` is the scheduler-owned running task, read with `SCHEDULER_LOCK`
    // held (its null case is checked below).
    let cur = unsafe { crate::task::current_task };
    if !cur.is_null() {
        // SAFETY: `cur` is the live running task (non-null checked here).
        let cur_ref = unsafe { &mut *cur };
        if cur_ref.priority > BOOST_TARGET && cur_ref.priority != MLFQ_LEVEL_RT {
            cur_ref.priority   = BOOST_TARGET;
            cur_ref.ticks_used = 0;
        }
    }
}

/// # Safety
///
/// Must be called from a task context with the scheduler free on this CPU; the
/// `SCHEDULE_IN_PROGRESS` guard makes a re-entrant call a no-op. It switches kernel stacks and
/// returns only when this task is scheduled again.
#[no_mangle]
pub unsafe extern "C" fn schedule() {
    if SCHEDULE_IN_PROGRESS
        .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        return;
    }

    if REAP_COUNTER.fetch_add(1, Ordering::Relaxed) % 128 == 127 {
        // SAFETY: reaps zombie tasks; the scheduler runs single-threaded per CPU here.
        unsafe { crate::task::task_reap() };
    }

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock and `schedule` is entered
    // without holding it (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `current_task` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let prev = unsafe { crate::task::current_task };
    // SAFETY: `task_list_head` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let head = unsafe { crate::task::task_list_head };
    if prev.is_null() || head.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        SCHEDULE_IN_PROGRESS.store(0, Ordering::Release);
        return;
    }

    // SAFETY: `prev` is the live running task (non-null checked above).
    let prev_state = unsafe { (*prev).state };
    if matches!(prev_state, TaskState::Zombie) {
        // SAFETY: `prev` is live, so its `proc` field is in bounds.
        let prev_proc = unsafe { (*prev).proc };
        // SAFETY: `prev_proc` is the live `ProcMeta` of the running task.
        let parent_pid = unsafe { (*prev_proc).parent_pid };
        if parent_pid != 0 {
            // SAFETY: signals the parent while `SCHEDULER_LOCK` is held.
            unsafe { crate::task::task_signal_locked(parent_pid, crate::task::SIGCHLD) };
            let parent = crate::task::find_task_by_pid(parent_pid);
            if !parent.is_null() {
                // SAFETY: `parent` is a live task (non-null checked here).
                let parent_state = unsafe { (*parent).state };
                if matches!(parent_state, TaskState::Waiting) {
                    // SAFETY: `parent` is live.
                    unsafe { (*parent).state = TaskState::Ready };
                    // SAFETY: `parent` is live.
                    let parent_pri = unsafe { (*parent).priority };
                    // SAFETY: `parent` is live and the lock is held.
                    unsafe { mlfq_enqueue_locked(parent, parent_pri) };
                }
            }
        }
    }

    wake_expired_sleepers();

    let next = pick_next_task();

    if next.is_null() || next == prev {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        SCHEDULE_IN_PROGRESS.store(0, Ordering::Release);
        return;
    }

    // SAFETY: `prev` is the live task being switched away from.
    let prev_state = unsafe { (*prev).state };
    match prev_state {
        TaskState::Running => {
            // SAFETY: `prev` is live.
            unsafe { (*prev).state = TaskState::Ready };
            // SAFETY: `prev` is live.
            let prev_pri = unsafe { (*prev).priority };
            // SAFETY: `prev` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(prev, prev_pri) };
        }
        TaskState::Sleeping => {
            // A task sleeping *with a deadline* is owned by the timer wheel,
            // which wakes it and enqueues it itself.  Only a task with no
            // deadline (blocked on a semaphore/mutex) belongs to the sleep
            // queue: pushing a wheel-tracked task there as well would leave it
            // in two intrusive lists chained through `queue_next`, and
            // `wake_expired_sleepers()` then walks the corrupted chain.
            // SAFETY: `prev` is live, so its `proc` field is in bounds.
            let prev_proc = unsafe { (*prev).proc };
            if prev_proc.is_null() {
                // SAFETY: `prev` is live and the lock is held.
                unsafe { mlfq_sleep_locked(prev) };
            } else {
                // SAFETY: `prev_proc` is the live `ProcMeta` of the running task.
                let prev_sleep_until = unsafe { (*prev_proc).sleep_until };
                if prev_sleep_until == 0 {
                    // SAFETY: `prev` is live and the lock is held.
                    unsafe { mlfq_sleep_locked(prev) };
                }
            }
        }
        TaskState::Waiting | TaskState::Zombie | TaskState::Stopped => {}
        TaskState::Ready => {}
    }

    // SAFETY: `next` is the live ready task just popped from the scheduler's own queues.
    unsafe { (*next).state = TaskState::Running };
    // SAFETY: `current_task` is the scheduler-owned current-task pointer, switched here under the
    // scheduler's own lock protocol.
    unsafe { crate::task::current_task = next };

    // SAFETY: `next` is live.
    let next_pid = unsafe { (*next).pid };
    crate::energy::observe_schedule(0, next_pid);

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `prev` is live.
    let prev_pd = unsafe { (*prev).page_directory };
    // SAFETY: `next` is live.
    let next_pd = unsafe { (*next).page_directory };

    // SAFETY: `page_directory` is the kernel's global page directory.
    let kernel_pd = unsafe { ffi::page_directory.get() };

    let effective_next_pd = if next_pd.is_null() { kernel_pd } else { next_pd };
    let effective_prev_pd = if prev_pd.is_null() { kernel_pd } else { prev_pd };

    // SAFETY: maps the kernel MMIO ranges into the incoming page directory.
    unsafe { cact_mm::vmm_sync_kernel_mmio_mappings(effective_next_pd) };

    if effective_next_pd != effective_prev_pd {
        // SAFETY: switches CR3 to the incoming live address space.
        unsafe { ffi::switch_paging(effective_next_pd) };
    }

    // SAFETY: `next` is live.
    let next_proc = unsafe { (*next).proc };
    if !next_proc.is_null() {
        // SAFETY: `next_proc` is the live `ProcMeta` of the incoming task.
        let stack_base = unsafe { (*next_proc).stack_base };
        if !stack_base.is_null() {
            let esp0 = stack_base as u32 + crate::task::KERNEL_STACK_SIZE as u32;
            // SAFETY: `tss_entry` is the kernel TSS, exclusively updated under `SCHEDULER_LOCK`.
            let tss_ptr = unsafe { ffi::tss_entry.get() };
            // SAFETY: `tss_ptr` is that global's address, used only here.
            let tss = unsafe { &mut *tss_ptr };
            tss.esp0 = esp0;
            // SAFETY: sets the ring-0 stack for the upcoming user entry.
            unsafe { ffi::syscall_set_esp0(esp0) };
        }
    }

    // SAFETY: disables interrupts around the context switch.
    unsafe { ffi::cli() };
    SCHEDULE_IN_PROGRESS.store(0, Ordering::Release);
    // SAFETY: `prev` is live; its saved-stack-pointer slot is handed to the switch routine.
    let prev_esp_slot = unsafe { core::ptr::addr_of_mut!((*prev).esp) };
    // SAFETY: `next` is live.
    let next_esp = unsafe { (*next).esp };
    // SAFETY: switches from `prev`'s saved stack to `next`'s; the lock is released and interrupts
    // off, which is the scheduler's core operation.
    unsafe { ffi::switch_to(prev_esp_slot, next_esp) };
    // SAFETY: re-enables interrupts, paired with the `cli` above.
    unsafe { ffi::sti() };

    // SAFETY: `current_task` is the scheduler-owned global, read after the switch.
    let cur = unsafe { crate::task::current_task };
    if !cur.is_null() {
        // SAFETY: `cur` is the live task now running.
        unsafe { crate::task::task_handle_signals(cur) };
    }
}

fn wake_expired_sleepers() {
    let now = crate::timer_wheel::timer_current_tick();
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; called from `schedule` with
    // `SCHEDULER_LOCK` held.
    let s = unsafe { &mut *mlfq_state_mut() };
    let sq = &mut s.sleep_queue;
    let mut prev: *mut TaskStruct = ptr::null_mut();
    let mut cur = sq.head;

    while !cur.is_null() {
        // SAFETY: `cur` was reached by walking the sleep queue's own chain, so it is a live
        // sleeping task.
        let next = unsafe { (*cur).queue_next };
        // SAFETY: `cur` is that live task, so its `proc` field is in bounds.
        let cur_proc = unsafe { (*cur).proc };
        // SAFETY: `cur_proc` is the task's live `ProcMeta`.
        let sleep_until = unsafe { (*cur_proc).sleep_until };
        if sleep_until != 0 && now >= sleep_until {
            if prev.is_null() {
                sq.head = next;
            } else {
                // SAFETY: `prev` was reached by walking this chain, so it is a live task.
                unsafe { (*prev).queue_next = next };
            }
            if sq.tail == cur {
                sq.tail = prev;
            }
            sq.count -= 1;

            // SAFETY: `cur_proc` is live.
            unsafe { (*cur_proc).sleep_until = 0 };
            // SAFETY: `cur` is live.
            unsafe { (*cur).state = TaskState::Ready };
            // SAFETY: `cur` is live.
            let priority = unsafe { (*cur).priority };
            // SAFETY: `cur` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(cur, priority) };
        } else {
            prev = cur;
        }
        cur = next;
    }
}

/// # Safety
///
/// Must be called only from the timer interrupt path, with the scheduler lock free.
#[no_mangle]
pub unsafe extern "C" fn on_timer_tick() {
    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the timer path holds no lock
    // when it calls in (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `current_task` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let cur = unsafe { crate::task::current_task };
    if cur.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return;
    }

    // SAFETY: `cur` is the live running task (non-null checked above).
    let cur_t = unsafe { &mut *cur };

    // Energy governor load sampling (master core, one sample per tick).
    crate::monitor::sample_tick();

    cur_t.ticks_used += 1;
    let quantum = MLFQ_QUANTUM[cur_t.priority.min(MLFQ_LEVELS as u32 - 1) as usize];

    let need_preempt = cur_t.ticks_used >= quantum;

    if need_preempt {
        if cur_t.priority != MLFQ_LEVEL_RT && cur_t.priority < MLFQ_LEVEL_BACKGROUND {
            cur_t.priority += 1;
        }
        cur_t.ticks_used = 0;
    }

    {
        // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`, mutated under
        // `SCHEDULER_LOCK`.
        let s = unsafe { &mut *mlfq_state_mut() };
        s.boost_counter = s.boost_counter.saturating_add(1);
        if s.boost_counter >= BOOST_INTERVAL {
            s.boost_counter = 0;
            do_priority_boost();
        }
    }

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // SAFETY: expires per-task timers; the scheduler state is reachable from the timer path.
    unsafe { crate::task::task_check_timers() };
    crate::task::task_check_kernel_stack();

    // SAFETY: `current_task` is the scheduler-owned global, read here.
    let live = unsafe { crate::task::current_task };
    if !live.is_null() {
        // SAFETY: `live` is the live task now running.
        unsafe { crate::task::task_handle_signals(live) };
    }

    // Energy decision engine pass (master core, once per tick).
    crate::decision::energy_decision_tick();

    // Load-balancing / migration pass (master core, once per tick).
    crate::balance::energy_balance_tick();

    if need_preempt {
        // SAFETY: `schedule` is the scheduler's core switch routine, called here with the lock
        // free.
        unsafe { schedule() };
    }
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct`, and the caller must not already hold
/// `SCHEDULER_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn mlfq_wake_task(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the caller must not hold it
    // (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    // SAFETY: `task` is non-null and live (see # Safety) and the lock is now held.
    unsafe { mlfq_wake_task_locked(task) };
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
}

/// State-aware wake for a task the caller has just decided to release.
/// A Sleeping task is *unlinked from the sleep queue first* — otherwise it ends
/// up in the sleep queue and a ready queue at once, both chained through
/// `queue_next`, and `wake_expired_sleepers()` walks the corrupted list.
/// Caller must hold [`SCHEDULER_LOCK`].
///
/// # Safety
///
/// `task` must be null or a live `TaskStruct`, and the caller must hold `SCHEDULER_LOCK`.
pub unsafe fn mlfq_wake_task_locked(task: *mut TaskStruct) {
    if task.is_null() {
        return;
    }
    // SAFETY: `task` is non-null and live (see # Safety); the caller holds `SCHEDULER_LOCK`.
    let state = unsafe { (*task).state };
    match state {
        TaskState::Sleeping => {
            mlfq_remove_from_sleep(task);
            // SAFETY: `task` is live.
            unsafe { (*task).state = TaskState::Ready };
            // SAFETY: `task` is live.
            let pri = unsafe { (*task).priority };
            // SAFETY: `task` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(task, pri) };
        }
        TaskState::Waiting | TaskState::Stopped => {
            // SAFETY: `task` is live.
            unsafe { (*task).state = TaskState::Ready };
            // SAFETY: `task` is live.
            let pri = unsafe { (*task).priority };
            // SAFETY: `task` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(task, pri) };
        }
        _ => {}
    }
}

/// `mlfq_wake_task_locked` for callers outside this crate that already hold
/// [`SCHEDULER_LOCK`] (the kernel sync primitives).
///
/// # Safety
///
/// `task` must be null or a live `TaskStruct`, and the caller must hold `SCHEDULER_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn sched_mlfq_wake_task_locked(task: *mut TaskStruct) {
    // SAFETY: Thin C shim: the caller holds `SCHEDULER_LOCK` and passes a live task pointer.
    unsafe {
        mlfq_wake_task_locked(task);
    }
}

/// # Safety
///
/// `task` must be null or a live `TaskStruct`, and the caller must hold `SCHEDULER_LOCK`.
pub unsafe fn task_voluntary_block(task: *mut TaskStruct, new_state: TaskState) {
    if task.is_null() {
        return;
    }
    // SAFETY: `task` is non-null and live (see # Safety); the caller holds `SCHEDULER_LOCK`, so
    // the exclusive reborrow for the priority/state updates is sound.
    let t = unsafe { &mut *task };
    let quantum = MLFQ_QUANTUM[t.priority.min(MLFQ_LEVELS as u32 - 1) as usize];
    if t.ticks_used < quantum / 2 + 1 && t.priority > MLFQ_LEVEL_INTERACTIVE {
        t.priority -= 1;
    }
    t.ticks_used = 0;
    t.state = new_state;
    if matches!(new_state, TaskState::Sleeping) {
        // SAFETY: `task` is live and the lock is held.
        unsafe { mlfq_sleep_locked(task) };
    }
}
