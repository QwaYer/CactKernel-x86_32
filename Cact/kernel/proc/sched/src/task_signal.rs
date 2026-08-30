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

#[no_mangle]
pub unsafe extern "C" fn task_kill(pid: u32) {
    if pid == 0 { return; }
    task_signal(pid, SIGKILL);
}

#[no_mangle]
pub unsafe extern "C" fn task_signal(pid: u32, signal: u32) {
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_signal_locked(pid, signal);
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);
}

#[no_mangle]
pub unsafe extern "C" fn task_signal_locked(pid: u32, signal: u32) {
    if task_list_head.is_null() || pid == 0 { return; }

    let t = find_task_by_pid(pid);
    if t.is_null() { return; }

    let p = (*t).proc;

    (*p).pending_signals |= signal;

    if signal & (SIGKILL | SIGSTOP) != 0 {
        match (*t).state {
            TaskState::Sleeping => {
                mlfq::mlfq_remove_from_sleep(t);
                (*t).state = TaskState::Ready;
                mlfq::mlfq_enqueue_locked(t, (*t).priority);
            }
            TaskState::Waiting => {
                (*t).state = TaskState::Ready;
                mlfq::mlfq_enqueue_locked(t, (*t).priority);
            }
            _ => {}
        }
        return;
    }

    if (*p).in_sigsuspend != 0
        && matches!((*t).state, TaskState::Sleeping)
        && (signal & !(*p).signal_mask) != 0
    {
        mlfq::mlfq_remove_from_sleep(t);
        (*t).state = TaskState::Ready;
        mlfq::mlfq_enqueue_locked(t, (*t).priority);
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_handle_signals(t: *mut TaskStruct) {
    if t.is_null() || (*t).proc.is_null() { return; }

    let p = (*t).proc;

    if (*p).pending_signals == 0 { return; }

    if (*p).in_sigsuspend != 0 {
        (*p).signal_mask   = (*p).saved_signal_mask;
        (*p).in_sigsuspend = 0;
    }

    if (*p).pending_signals & SIGKILL != 0 {
        (*p).pending_signals = 0;
        task_signal_locked((*p).parent_pid, SIGCHLD);
        (*t).state = TaskState::Zombie;
        return;
    }

    if (*p).pending_signals & SIGSTOP != 0 {
        (*p).pending_signals &= !SIGSTOP;
        (*t).state = TaskState::Sleeping;
        return;
    }

    let deliverable = (*p).pending_signals & !(*p).signal_mask;
    if deliverable == 0 { return; }

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
    unsafe {
        let p = (*t).proc;
        if matches!((*t).state, TaskState::Zombie) {
            return;
        }

        (*p).pending_signals &= !sig;
        let handler = (*p).signal_handlers[handler_idx];

        if sig == SIGCONT {
            if matches!((*t).state, TaskState::Sleeping) {
                (*t).state = TaskState::Ready;
            }
            return;
        }

        if sig == SIGCHLD || sig == SIGWINCH {
            if handler != SIG_DFL && handler != SIG_IGN {
                (*p).pending_signals |= sig;
            }
            return;
        }

        if term_by_default && (handler == SIG_DFL || handler == SIG_IGN) {
            task_signal_locked((*p).parent_pid, SIGCHLD);
            (*t).state = TaskState::Zombie;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_sigaction(
    t:       *mut TaskStruct,
    signum:  u32,
    handler: u32,
) -> i32 {
    if t.is_null() || (*t).proc.is_null() || signum >= NSIG as u32 || signum == 0 { return -1; }

    let p = (*t).proc;

    if handler != SIG_DFL && handler != SIG_IGN && handler >= KERNEL_BASE {
        return -1;
    }

    if (1u32 << signum) & SIG_UNCATCHABLE != 0 { return -1; }

    (*p).signal_handlers[signum as usize] = handler;
    0
}

#[no_mangle]
pub unsafe extern "C" fn task_sigprocmask(
    t:      *mut TaskStruct,
    how:    i32,
    set:    *const u32,
    oldset: *mut u32,
) -> i32 {
    if t.is_null() || (*t).proc.is_null() { return -1; }
    let p = (*t).proc;
    if !oldset.is_null() { *oldset = (*p).signal_mask; }
    if set.is_null() { return 0; }

    let new_mask = *set & !SIG_UNCATCHABLE;
    match how {
        0 => (*p).signal_mask |=  new_mask,
        1 => (*p).signal_mask &= !new_mask,
        2 => (*p).signal_mask  =  new_mask,
        _ => return -1,
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn task_check_timers() {
    timer_wheel::timer_wheel_tick();
    check_alarm_timers();
}

fn check_alarm_timers() {
    let now = timer_wheel::timer_current_tick();

    unsafe {
        irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

        let mut cur = task_list_head;
        while !cur.is_null() {
            let next = (*cur).next;
            let p = (*cur).proc;
            if p.is_null() { cur = next; continue; }

            if (*p).alarm_ticks != 0 && now >= (*p).alarm_ticks {
                (*p).alarm_ticks = 0;
                (*p).pending_signals |= SIGALRM;
                wake_if_sigsuspend(cur, SIGALRM);
            }

            if (*p).itimer_value != 0 && now >= (*p).itimer_value {
                (*p).pending_signals |= SIGALRM;
                (*p).itimer_value = if (*p).itimer_interval != 0 {
                    now + (*p).itimer_interval
                } else {
                    0
                };
                wake_if_sigsuspend(cur, SIGALRM);
            }

            cur = next;
        }

        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
    }
}

fn wake_if_sigsuspend(t: *mut TaskStruct, signal: u32) {
    if t.is_null() {
        return;
    }
    unsafe {
        let p = (*t).proc;
        if (*p).in_sigsuspend != 0
            && matches!((*t).state, TaskState::Sleeping)
            && (signal & !(*p).signal_mask) != 0
        {
            mlfq::mlfq_remove_from_sleep(t);
            (*t).state = TaskState::Ready;
            mlfq::mlfq_enqueue_locked(t, (*t).priority);
        }
    }
}
