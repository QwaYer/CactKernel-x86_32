//! Signal delivery, sigaction/sigprocmask syscall support, and alarm/itimer
//! bookkeeping.

use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::timer_wheel;
use crate::task::{
    find_task_by_pid, task_list_head, TaskStruct, TaskState, SCHEDULER_LOCK,
    KERNEL_BASE, NSIG, SIGALRM, SIGCHLD, SIGCONT, SIGFPE, SIGHUP, SIGINT, SIGKILL,
    SIGQUIT, SIGSEGV, SIGTERM, SIGSTOP, SIGWINCH, SIG_DFL, SIG_IGN, SIG_UNCATCHABLE,
};

/// # Safety
///
/// Must be called from kernel context with `SCHEDULER_LOCK` free.
#[no_mangle]
pub unsafe extern "C" fn task_kill(pid: u32) {
    // SAFETY: Delegates to `task_signal`, which takes `SCHEDULER_LOCK` itself.
    unsafe {
        if pid == 0 { return; }
        task_signal(pid, SIGKILL);
    }
}

/// # Safety
///
/// Must be called from kernel context with `SCHEDULER_LOCK` free.
#[no_mangle]
pub unsafe extern "C" fn task_signal(pid: u32, signal: u32) {
    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the caller must not hold it.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    // SAFETY: `task_signal_locked` requires `SCHEDULER_LOCK`, which is held here.
    unsafe { task_signal_locked(pid, signal) };
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
}

/// Non-zero when the *current* task has a signal that is pending and not blocked
/// by its mask — i.e. one that will be delivered as soon as this task returns to
/// userspace.
///
/// Blocking syscalls (socket read/write) poll this and return `-EINTR` instead
/// of sleeping on.  Signals are only acted on when a task reaches userspace or
/// the scheduler, so without this a task stuck in a blocking `read()` could not
/// be killed with Ctrl+C at all.
///
/// # Safety
///
/// Must be called from a task context with `SCHEDULER_LOCK` free (it only reads).
#[no_mangle]
pub unsafe extern "C" fn task_signal_pending_current() -> u32 {
    // SAFETY: `current_task` is a scheduler-owned global; this function only reads it.
    let t = unsafe { crate::task::current_task };
    if t.is_null() {
        return 0;
    }
    // SAFETY: `t` is the live current task (non-null checked above).
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() {
        return 0;
    }
    // SAFETY: `proc_ptr` is the live `ProcMeta` of the current task.
    let p = unsafe { &*proc_ptr };
    p.pending_signals & !p.signal_mask
}

/// # Safety
///
/// Must be called with `SCHEDULER_LOCK` held.
#[no_mangle]
pub unsafe extern "C" fn task_signal_locked(pid: u32, signal: u32) {
    // SAFETY: `task_list_head` is a scheduler-owned global, read with the caller's
    // `SCHEDULER_LOCK` held (see # Safety).
    let head = unsafe { task_list_head };
    if head.is_null() || pid == 0 {
        return;
    }

    let t = find_task_by_pid(pid);
    if t.is_null() {
        return;
    }

    // SAFETY: `t` is a live task found in the task list (non-null checked above).
    let p = unsafe { (*t).proc };
    // SAFETY: `p` is that task's live `ProcMeta`.
    unsafe { (*p).pending_signals |= signal };

    if signal & (SIGKILL | SIGSTOP) != 0 {
        // SAFETY: `t` is live; the caller holds `SCHEDULER_LOCK`.
        unsafe { mlfq::mlfq_wake_task_locked(t) };
        return;
    }

    // SAFETY: `t` is live.
    let t_state = unsafe { (*t).state };
    if signal & SIGCONT != 0 && matches!(t_state, TaskState::Stopped) {
        // SAFETY: `t` is live; the caller holds `SCHEDULER_LOCK`.
        unsafe { mlfq::mlfq_wake_task_locked(t) };
        return;
    }

    // SAFETY: `p` is the live `ProcMeta` of `t`.
    let in_sigsuspend = unsafe { (*p).in_sigsuspend };
    // SAFETY: `p` is live.
    let signal_mask = unsafe { (*p).signal_mask };
    // SAFETY: `t` is live.
    let t_state = unsafe { (*t).state };
    if in_sigsuspend != 0
        && matches!(t_state, TaskState::Sleeping)
        && (signal & !signal_mask) != 0
    {
        mlfq::mlfq_remove_from_sleep(t);
        // SAFETY: `t` is live.
        unsafe { (*t).state = TaskState::Ready };
        // SAFETY: `t` is live.
        let pri = unsafe { (*t).priority };
        // SAFETY: `t` is live; the caller holds `SCHEDULER_LOCK`.
        unsafe { mlfq::mlfq_enqueue_locked(t, pri) };
    }
}

/// # Safety
///
/// `t` must be null or a live `TaskStruct`; must be called from kernel context.
#[no_mangle]
pub unsafe extern "C" fn task_handle_signals(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is the live task being delivered to (see # Safety).
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() {
        return;
    }
    // SAFETY: `proc_ptr` is that task's live `ProcMeta`; this runs in the task's own context and
    // the reference is not live across the `handle_signal_bit` calls at the end.
    let p = unsafe { &mut *proc_ptr };

    if p.pending_signals == 0 {
        return;
    }

    if p.in_sigsuspend != 0 {
        p.signal_mask   = p.saved_signal_mask;
        p.in_sigsuspend = 0;
    }

    if p.pending_signals & SIGKILL != 0 {
        p.pending_signals = 0;
        let parent_pid = p.parent_pid;
        // SAFETY: signalling the parent requires `SCHEDULER_LOCK`, held by the caller.
        unsafe { task_signal_locked(parent_pid, SIGCHLD) };
        // SAFETY: `t` is live.
        unsafe { (*t).state = TaskState::Zombie };
        return;
    }

    if p.pending_signals & SIGSTOP != 0 {
        p.pending_signals &= !SIGSTOP;
        // SAFETY: `t` is live.
        unsafe { (*t).state = TaskState::Stopped };
        // SAFETY: `t` is live.
        unsafe { notify_parent_of_stop(t) };
        // SAFETY: yields the CPU; the scheduler's lock protocol is respected by `schedule`.
        unsafe { crate::mlfq::schedule() };
        return;
    }

    let deliverable = p.pending_signals & !p.signal_mask;
    if deliverable == 0 {
        return;
    }

    handle_signal_bit(t, deliverable, SIGTERM,  1,  true);
    handle_signal_bit(t, deliverable, SIGCONT,  3,  false);
    handle_signal_bit(t, deliverable, SIGALRM,  5,  true);
    handle_signal_bit(t, deliverable, SIGCHLD,  6,  false);
    handle_signal_bit(t, deliverable, SIGFPE,   7,  true);
    handle_signal_bit(t, deliverable, SIGSEGV,  8,  true);
    handle_signal_bit(t, deliverable, SIGWINCH, 9,  false);
    handle_signal_bit(t, deliverable, SIGHUP,   10, true);
    handle_signal_bit(t, deliverable, SIGINT,   11, true);
    handle_signal_bit(t, deliverable, SIGQUIT,  12, true);
}

/// A stopped child must be reported to its parent or `waitpid(…, WUNTRACED)`
/// would sleep forever: the child is not a zombie, so nothing else wakes it.
unsafe fn notify_parent_of_stop(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is live (non-null checked above), so its `proc` field is in bounds.
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() {
        return;
    }
    // SAFETY: `proc_ptr` is that task's live `ProcMeta`; the reference is not live across the
    // calls below, which never touch this task's own proc.
    let parent_pid = unsafe { (*proc_ptr).parent_pid };

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; this runs with it free.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    if parent_pid != 0 {
        // SAFETY: signalling the parent requires the lock, which is held here.
        unsafe { task_signal_locked(parent_pid, SIGCHLD) };
        let parent = find_task_by_pid(parent_pid);
        if !parent.is_null() {
            // SAFETY: `parent` is a live task (non-null checked here).
            let parent_state = unsafe { (*parent).state };
            if matches!(parent_state, TaskState::Waiting) {
                // SAFETY: `parent` is live.
                unsafe { (*parent).state = TaskState::Ready };
                // SAFETY: `parent` is live.
                let pri = unsafe { (*parent).priority };
                // SAFETY: `parent` is live and the lock is held.
                unsafe { mlfq::mlfq_enqueue_locked(parent, pri) };
            }
        }
    }
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
}

fn handle_signal_bit(
    t:          *mut TaskStruct,
    deliverable: u32,
    sig:        u32,
    handler_idx: usize,
    term_by_default: bool,
) {
    if t.is_null() {
        return;
    }
    if deliverable & sig == 0 {
        return;
    }
    // SAFETY: `t` is non-null (checked by the caller) and, per `task_handle_signals`' contract,
    // is the live task currently being delivered to.
    let proc_ptr = unsafe { (*t).proc };
    // SAFETY: `t` is that live task.
    let t_state = unsafe { (*t).state };
    if matches!(t_state, TaskState::Zombie) {
        return;
    }
    // SAFETY: `proc_ptr` is that task's live `ProcMeta`; the reference is not live across the
    // `task_signal_locked` call at the end.
    let p = unsafe { &mut *proc_ptr };

    p.pending_signals &= !sig;
    let handler = p.signal_handlers[handler_idx];

    if sig == SIGCONT {
        // SAFETY: `t` is live.
        let t_state = unsafe { (*t).state };
        if matches!(t_state, TaskState::Sleeping) {
            // SAFETY: `t` is live.
            unsafe { (*t).state = TaskState::Ready };
        }
        return;
    }

    if sig == SIGCHLD || sig == SIGWINCH {
        if handler != SIG_DFL && handler != SIG_IGN {
            p.pending_signals |= sig;
        }
        return;
    }

    if term_by_default && (handler == SIG_DFL || handler == SIG_IGN) {
        let parent_pid = p.parent_pid;
        // SAFETY: signalling the parent requires `SCHEDULER_LOCK`, held by the caller.
        unsafe { task_signal_locked(parent_pid, SIGCHLD) };
        // SAFETY: `t` is live.
        unsafe { (*t).state = TaskState::Zombie };
    }
}

/// # Safety
///
/// `t` must be null or a live `TaskStruct`; must be called from kernel context.
#[no_mangle]
pub unsafe extern "C" fn task_sigaction(
    t:       *mut TaskStruct,
    signum:  u32,
    handler: u32,
) -> i32 {
    if t.is_null() {
        return -1;
    }
    // SAFETY: `t` is live (non-null checked above), so its `proc` field is in bounds.
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() || signum >= NSIG as u32 || signum == 0 {
        return -1;
    }

    if handler != SIG_DFL && handler != SIG_IGN && handler >= KERNEL_BASE {
        return -1;
    }

    if (1u32 << signum) & SIG_UNCATCHABLE != 0 {
        return -1;
    }

    // SAFETY: `proc_ptr` is the task's live `ProcMeta`; `signum` was bounded against `NSIG`
    // above, so the handler write is in bounds.
    let p = unsafe { &mut *proc_ptr };
    p.signal_handlers[signum as usize] = handler;
    0
}

/// # Safety
///
/// `t` must be null or a live `TaskStruct`; `set`/`oldset` must be null or point to a live
/// `u32`.
#[no_mangle]
pub unsafe extern "C" fn task_sigprocmask(
    t:      *mut TaskStruct,
    how:    i32,
    set:    *const u32,
    oldset: *mut u32,
) -> i32 {
    if t.is_null() {
        return -1;
    }
    // SAFETY: `t` is live (non-null checked above), so its `proc` field is in bounds.
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() {
        return -1;
    }
    // SAFETY: `proc_ptr` is the task's live `ProcMeta`.
    let p = unsafe { &mut *proc_ptr };
    if !oldset.is_null() {
        // SAFETY: `oldset` is non-null (checked here) and points to a live `u32` (caller
        // contract).
        unsafe { *oldset = p.signal_mask };
    }
    if set.is_null() {
        return 0;
    }

    // SAFETY: `set` is non-null (checked above) and points to a live `u32` (caller contract).
    let new_mask = unsafe { *set } & !SIG_UNCATCHABLE;
    match how {
        0 => p.signal_mask |=  new_mask,
        1 => p.signal_mask &= !new_mask,
        2 => p.signal_mask  =  new_mask,
        _ => return -1,
    }
    0
}

/// # Safety
///
/// Must be called with `SCHEDULER_LOCK` free; it takes the lock itself inside the timer-wheel
/// path.
#[no_mangle]
pub unsafe extern "C" fn task_check_timers() {
    timer_wheel::timer_wheel_tick();
    check_alarm_timers();
}

fn check_alarm_timers() {
    let now = timer_wheel::timer_current_tick();

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; this runs with it free.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `task_list_head` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let mut cur = unsafe { task_list_head };
    while !cur.is_null() {
        // SAFETY: `cur` is a live task on the task list (the lock is held).
        let next = unsafe { (*cur).next };
        // SAFETY: `cur` is live, so its `proc` field is in bounds.
        let p = unsafe { (*cur).proc };
        if p.is_null() {
            cur = next;
            continue;
        }

        // SAFETY: `p` is the live `ProcMeta` of `cur`.
        let alarm_ticks = unsafe { (*p).alarm_ticks };
        if alarm_ticks != 0 && now >= alarm_ticks {
            // SAFETY: `p` is live.
            unsafe { (*p).alarm_ticks = 0 };
            // SAFETY: `p` is live.
            unsafe { (*p).pending_signals |= SIGALRM };
            wake_if_sigsuspend(cur, SIGALRM);
        }

        // SAFETY: `p` is live.
        let itimer_value = unsafe { (*p).itimer_value };
        if itimer_value != 0 && now >= itimer_value {
            // SAFETY: `p` is live.
            unsafe { (*p).pending_signals |= SIGALRM };
            // SAFETY: `p` is live.
            let itimer_interval = unsafe { (*p).itimer_interval };
            let next_value = if itimer_interval != 0 {
                now + itimer_interval
            } else {
                0
            };
            // SAFETY: `p` is live.
            unsafe { (*p).itimer_value = next_value };
            wake_if_sigsuspend(cur, SIGALRM);
        }

        cur = next;
    }

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
}

fn wake_if_sigsuspend(t: *mut TaskStruct, signal: u32) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is non-null and a live `TaskStruct` (per the caller), so its `proc` field is in
    // bounds.
    let proc_ptr = unsafe { (*t).proc };
    // SAFETY: `proc_ptr` is that task's live `ProcMeta`.
    let in_sigsuspend = unsafe { (*proc_ptr).in_sigsuspend };
    // SAFETY: `t` is live.
    let t_state = unsafe { (*t).state };
    // SAFETY: `proc_ptr` is live.
    let signal_mask = unsafe { (*proc_ptr).signal_mask };
    if in_sigsuspend != 0
        && matches!(t_state, TaskState::Sleeping)
        && (signal & !signal_mask) != 0
    {
        mlfq::mlfq_remove_from_sleep(t);
        // SAFETY: `t` is live.
        unsafe { (*t).state = TaskState::Ready };
        // SAFETY: `t` is live.
        let pri = unsafe { (*t).priority };
        // SAFETY: `t` is live and `SCHEDULER_LOCK` is held by the caller.
        unsafe { mlfq::mlfq_enqueue_locked(t, pri) };
    }
}
