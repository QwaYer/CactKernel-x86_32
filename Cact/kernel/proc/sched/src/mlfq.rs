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
use crate::intrusive_queue::{IntrusiveQueue, Link};

// The MLFQ policy constants (levels, quanta, boost target) live in the pure
// `mlfq_policy` module; re-export them so the existing `crate::mlfq::MLFQ_*`
// call sites keep working.
pub use crate::mlfq_policy::{
    MLFQ_LEVELS, MLFQ_LEVEL_BACKGROUND, MLFQ_LEVEL_INTERACTIVE, MLFQ_LEVEL_NORMAL,
    MLFQ_LEVEL_RT, MLFQ_QUANTUM, BOOST_TARGET,
};

// The ready/sleep queues are intrusive FIFOs of `TaskStruct` whose link is
// `queue_next`; the mechanics come from the pure `intrusive_queue` module.
impl Link for TaskStruct {
    #[inline]
    fn next(&self) -> *mut Self {
        self.queue_next
    }
    #[inline]
    fn set_next(&mut self, next: *mut Self) {
        self.queue_next = next;
    }
}

type MlfqQueue = IntrusiveQueue<TaskStruct>;

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
                IntrusiveQueue::new(),
                IntrusiveQueue::new(),
                IntrusiveQueue::new(),
                IntrusiveQueue::new(),
            ],
            sleep_queue:   IntrusiveQueue::new(),
            boost_counter: 0,
        }
    }
}

static MLFQ_STATE: SyncUnsafeCell<MlfqState> = SyncUnsafeCell::new(MlfqState::new());

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
        *s = MlfqState::new();
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
        s.queues.iter().map(|q| q.count()).sum()
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
            .find(|&l| !s.queues[l].is_empty())
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

/// Unlink `task` from the blocked queue.  Returns `true` if it was there, i.e.
/// if the task had already parked (see `sched_park_prev`).
pub fn mlfq_remove_from_sleep(task: *mut TaskStruct) -> bool {
    // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`; callers hold `SCHEDULER_LOCK`, so
    // the unlink from the sleep queue cannot race another CPU.
    unsafe {
        let s = &mut *mlfq_state_mut();
        s.sleep_queue.remove(task)
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
    let cur = crate::task::current_task();
    if !cur.is_null() {
        // SAFETY: `cur` is the live running task (non-null checked here).
        let cur_ref = unsafe { &mut *cur };
        if cur_ref.priority > BOOST_TARGET && cur_ref.priority != MLFQ_LEVEL_RT {
            cur_ref.priority   = BOOST_TARGET;
            cur_ref.ticks_used = 0;
        }
    }
}

/// Park the task `switch_to` just switched away from.
///
/// This is the second half of `schedule()`: the outgoing task is made runnable
/// again only *after* `switch_to` has committed the switch and saved its stack
/// pointer (`mov [prev_esp], esp`), so no other CPU can pop it and resume it on
/// a stale stack.  Called from `switch_to` on the incoming task's stack, with
/// interrupts disabled.
///
/// # Safety
///
/// `prev` must be null or the live `TaskStruct` whose `esp` `switch_to` has
/// just saved; the caller must not hold `SCHEDULER_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn sched_park_prev(prev: *mut TaskStruct) {
    if prev.is_null() {
        return;
    }
    // This core's idle context is never queued (see `schedule`), so there is
    // nothing to park: it resumes the next time this core goes idle.
    if prev == crate::percpu::idle() {
        return;
    }
    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; `switch_to` released it
    // before the switch (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `prev` is the live task just switched away from (see # Safety); the lock is held.
    let state = unsafe { (*prev).state };
    match state {
        TaskState::Running => {
            // Preempted while still runnable: put it back on a ready queue.
            // SAFETY: `prev` is live.
            unsafe { (*prev).state = TaskState::Ready };
            // SAFETY: `prev` is live.
            let prev_pri = unsafe { (*prev).priority };
            // SAFETY: `prev` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(prev, prev_pri) };
        }
        TaskState::Sleeping => {
            // A task is in a blocking structure only after it has parked.  A
            // deadline sleep enters the timer wheel here; a deadlineless block
            // (semaphore/mutex) enters the blocked queue.  Wakers rely on that
            // invariant to never queue a task another core is still running.
            // SAFETY: `prev` is live, so its `proc` field is in bounds.
            let prev_proc = unsafe { (*prev).proc };
            let sleep_until = if prev_proc.is_null() {
                0
            } else {
                // SAFETY: `prev_proc` is the live `ProcMeta` (null-checked just above).
                unsafe { (*prev_proc).sleep_until }
            };
            if sleep_until == 0 {
                // SAFETY: `prev` is live and the lock is held.
                unsafe { mlfq_sleep_locked(prev) };
            } else {
                // SAFETY: `prev` is live with a non-null `proc`, and the lock is held.
                unsafe { crate::timer_wheel::timer_wheel_insert(prev) };
            }
        }
        TaskState::Waiting | TaskState::Stopped => {
            // Blocked with no deadline (waitpid / SIGSTOP): the blocked queue,
            // same "parked" invariant as a semaphore wait.
            // SAFETY: `prev` is live and the lock is held.
            unsafe { mlfq_sleep_locked(prev) };
        }
        TaskState::Ready => {
            // A waker made us runnable while we were still on the CPU and
            // deliberately did not queue us; we are parked now, so queue here.
            // SAFETY: `prev` is live.
            let prev_pri = unsafe { (*prev).priority };
            // SAFETY: `prev` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(prev, prev_pri) };
        }
        TaskState::Zombie => {}
    }

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
}

/// # Safety
///
/// Must be called from a task context with the scheduler free on this CPU; the
/// `SCHEDULE_IN_PROGRESS` guard makes a re-entrant call a no-op. It switches kernel stacks and
/// returns only when this task is scheduled again.
#[no_mangle]
pub unsafe extern "C" fn schedule() {
    if crate::percpu::schedule_guard()
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
    let prev = crate::task::current_task();
    // SAFETY: `task_list_head` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let head = unsafe { crate::task::task_list_head };
    if prev.is_null() || head.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        crate::percpu::schedule_guard().store(0, Ordering::Release);
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
                    // Only queue a parked parent; a still-running one is left
                    // Ready and its own park step queues it.
                    let parent_parked = mlfq_remove_from_sleep(parent);
                    // SAFETY: `parent` is live.
                    unsafe { (*parent).state = TaskState::Ready };
                    if parent_parked {
                        // SAFETY: `parent` is live.
                        let parent_pri = unsafe { (*parent).priority };
                        // SAFETY: `parent` is live and the lock is held.
                        unsafe { mlfq_enqueue_locked(parent, parent_pri) };
                    }
                }
            }
        }
    }

    wake_expired_sleepers();

    let mut next = pick_next_task();

    if next.is_null() {
        // Nothing else is runnable.  A still-Running `prev` simply keeps the
        // CPU (a Running task belongs on no queue); otherwise hand the CPU to
        // this core's own idle context.  Idle tasks are CPU-pinned and never
        // queued — each lives on its own core's stack, so letting another core
        // run it would alias that stack.
        // SAFETY: `prev` is the live running task (non-null checked above).
        let prev_now = unsafe { (*prev).state };
        if matches!(prev_now, TaskState::Running) {
            // SAFETY: the lock was acquired above.
            unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
            crate::percpu::schedule_guard().store(0, Ordering::Release);
            return;
        }
        next = crate::percpu::idle();
    }

    if next.is_null() || next == prev {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        crate::percpu::schedule_guard().store(0, Ordering::Release);
        return;
    }

    // `prev` is NOT parked here: it must stay off the shared ready queue until
    // `switch_to` has saved its stack pointer, or another CPU could pop it and
    // resume it on a stale stack.  `switch_to` calls `sched_park_prev` for that
    // once the switch is committed.

    // SAFETY: `next` is the live ready task just popped from the scheduler's own queues.
    unsafe { (*next).state = TaskState::Running };
    // Switches this CPU's running-task pointer under the scheduler's own lock protocol.
    crate::task::set_current_task(next);

    // SAFETY: `next` is live.
    let next_pid = unsafe { (*next).pid };
    let me = crate::percpu::this_cpu() as u32;
    crate::energy::observe_schedule(me, next_pid);

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
            // The ring-0 stack for the next task, set on THIS core: the BSP's TSS
            // is the C `tss_entry`, each AP's is its own `SmpCpu` slot.  Both are
            // used by the CPU on ring3->ring0 transitions (interrupts, faults),
            // so they must track the running task per core.
            let me = crate::percpu::this_cpu();
            if me == 0 {
                // SAFETY: `tss_entry` is the BSP TSS, exclusively updated under `SCHEDULER_LOCK`.
                let tss_ptr = unsafe { ffi::tss_entry.get() };
                // SAFETY: `tss_ptr` is that global's address, used only here.
                let tss = unsafe { &mut *tss_ptr };
                tss.esp0 = esp0;
            } else {
                crate::smp::set_tss_esp0(me, esp0);
            }
            // SAFETY: sets the ring-0 stack for the upcoming user entry in this
            // core's own SYSENTER ESP MSR.
            unsafe { ffi::syscall_set_esp0(esp0) };
            // SAFETY: value-only setter for this CPU's slot in the per-CPU
            // AMD-SYSCALL entry-stack table.
            unsafe { ffi::syscall_set_cpu_esp0(me as u32, esp0) };
        }
    }

    // SAFETY: disables interrupts around the context switch.
    unsafe { ffi::cli() };
    crate::percpu::schedule_guard().store(0, Ordering::Release);
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
    let cur = crate::task::current_task();
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

    // Drain and re-queue: expired sleepers go onto their MLFQ level, the rest are
    // appended back (order preserved).  The pass must process exactly the nodes
    // queued when it started -- pushing a not-yet-expired node back and popping
    // it again would spin forever -- so take a count snapshot first.  The ready
    // queues are reached through the same static; each access re-borrows from the
    // raw pointer, sound because the caller holds `SCHEDULER_LOCK`.
    let mut remaining = sq.count();
    while remaining > 0 {
        remaining -= 1;
        let cur = sq.pop();
        if cur.is_null() {
            break;
        }
        // SAFETY: `cur` is a live task just popped from this queue, so its
        // `proc` field is in bounds.
        let cur_proc = unsafe { (*cur).proc };
        let sleep_until = if cur_proc.is_null() {
            0
        } else {
            // SAFETY: `cur_proc` is the task's live `ProcMeta`.
            unsafe { (*cur_proc).sleep_until }
        };
        if sleep_until != 0 && now >= sleep_until {
            if !cur_proc.is_null() {
                // SAFETY: `cur_proc` is live.
                unsafe { (*cur_proc).sleep_until = 0 };
            }
            // SAFETY: `cur` is live.
            unsafe { (*cur).state = TaskState::Ready };
            // SAFETY: `cur` is live.
            let priority = unsafe { (*cur).priority };
            // SAFETY: `cur` is live and the lock is held.
            unsafe { mlfq_enqueue_locked(cur, priority) };
        } else {
            // SAFETY: `cur` is a live task still owned by the sleep queue.
            sq.push(unsafe { &mut *cur });
        }
    }
}

/// # Safety
///
/// Must be called only from the timer interrupt path, with the scheduler lock free.
#[no_mangle]
pub unsafe extern "C" fn on_timer_tick() {
    // Global scheduler state (queues, boost, timer wheel, governor) is owned by
    // the master core; worker cores run only the per-CPU parts of the tick.
    let me = crate::percpu::this_cpu();

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the timer path holds no lock
    // when it calls in (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `current_task` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let cur = crate::task::current_task();
    if cur.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return;
    }

    // SAFETY: `cur` is the live running task (non-null checked above).
    let cur_t = unsafe { &mut *cur };

    // Every core samples its own load (so the governor has real per-core
    // numbers).  A saturated worker that still sees queued work asks the master
    // for help — the governor's reverse direction.
    const HELP_LOAD_PERMILLE: u32 = 850;
    let load = crate::monitor::sample_tick();
    if me != 0 && load >= HELP_LOAD_PERMILLE && mlfq_runnable_count() > 0 {
        crate::monitor::request_help(me as u32);
    }

    cur_t.ticks_used += 1;
    let quantum = crate::mlfq_policy::quantum_for(cur_t.priority);

    let need_preempt = cur_t.ticks_used >= quantum;

    if need_preempt {
        cur_t.priority = crate::mlfq_policy::demote_on_quantum(cur_t.priority);
        cur_t.ticks_used = 0;
    }

    if me == 0 {
        // SAFETY: `mlfq_state_mut` yields the static `MLFQ_STATE`, mutated under
        // `SCHEDULER_LOCK`.
        let s = unsafe { &mut *mlfq_state_mut() };
        s.boost_counter = s.boost_counter.saturating_add(1);
        if crate::mlfq_policy::boost_due(s.boost_counter) {
            s.boost_counter = 0;
            do_priority_boost();
        }
    }

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // Global timekeeping (the timer wheel) is advanced on the master core only.
    if me == 0 {
        // SAFETY: expires per-task timers; the scheduler state is reachable from the timer path.
        unsafe { crate::task::task_check_timers() };
    }
    crate::task::task_check_kernel_stack();

    // SAFETY: `current_task` is the scheduler-owned global, read here.
    let live = crate::task::current_task();
    if !live.is_null() {
        // SAFETY: `live` is the live task now running.
        unsafe { crate::task::task_handle_signals(live) };
    }

    if me == 0 {
        // Energy decision engine pass (master core, once per tick).
        crate::decision::energy_decision_tick();

        // Load-balancing / migration pass (master core, once per tick).
        crate::balance::energy_balance_tick();
    }

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
            // A task sits in a blocking structure only once it has parked.  If
            // it is still running on its way to block it is in neither, so it is
            // *not* safe to queue it here (another core is still running it):
            // just mark it Ready and let its own `sched_park_prev` queue it.
            let parked = mlfq_remove_from_sleep(task)
                || {
                    // SAFETY: `task` is a live Sleeping task with a `ProcMeta`, and the caller
                    // holds `SCHEDULER_LOCK`.
                    unsafe { crate::timer_wheel::timer_wheel_remove(task) }
                };
            // SAFETY: `task` is live.
            unsafe { (*task).state = TaskState::Ready };
            if parked {
                // SAFETY: `task` is live.
                let pri = unsafe { (*task).priority };
                // SAFETY: `task` is live and the lock is held.
                unsafe { mlfq_enqueue_locked(task, pri) };
            }
        }
        TaskState::Waiting | TaskState::Stopped => {
            // Parked Waiting/Stopped tasks live in the blocked queue too.
            let parked = mlfq_remove_from_sleep(task);
            // SAFETY: `task` is live.
            unsafe { (*task).state = TaskState::Ready };
            if parked {
                // SAFETY: `task` is live.
                let pri = unsafe { (*task).priority };
                // SAFETY: `task` is live and the lock is held.
                unsafe { mlfq_enqueue_locked(task, pri) };
            }
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
    let quantum = crate::mlfq_policy::quantum_for(t.priority);
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
