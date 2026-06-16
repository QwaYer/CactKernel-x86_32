#include "signal.h"
#include "validate.h"
#include "helper.h"

// Timer frequency for alarm/setitimer conversions
#define TIMER_HZ_SIGNALS 100

// Check whether the current task is allowed to signal the task with |pid|.
// Returns 0 if permitted, -1 if denied.
static int _signal_permitted(uint32_t pid) {
    if (!current_task) return -1;
    if (current_task->proc->euid == 0) return 0;  // root bypass

    extern struct task_struct* volatile task_list_head;
    struct task_struct *t = task_list_head;
    while (t) {
        if (t->pid == pid) {
            if (t->proc && t->proc->uid == current_task->proc->uid)
                return 0;
            return -1;
        }
        t = t->next;
    }
    return -1;  // target not found
}

// kill() — send a signal to a process (simplified: always SIGKILL)
static int sys_kill_impl(uint32_t pid) {
    if (_signal_permitted(pid) < 0) return -1;
    task_kill(pid);
    return 0;
}

// signal() — send any signal to a process by PID
static int sys_signal_impl(uint32_t pid, uint32_t signal) {
    if (!pid) return -1;
    if (_signal_permitted(pid) < 0) return -1;
    task_signal(pid, signal);
    return 0;
}

// sigaction() — set the handler for a signal
static int sys_sigaction_impl(struct syscall_frame* regs) {
    uint32_t signum  = regs->ebx;
    uint32_t handler = regs->ecx;

    if (!current_task) return -1;

    // Reject handlers that lie in kernel space (C-07 fix)
    if (handler > SIG_IGN && (handler < USER_SPACE_START || handler >= KERNEL_BASE)) return -1;

    return task_sigaction(current_task, signum, handler);
}

// sigprocmask() — examine or change the signal mask
static int sys_sigprocmask_impl(struct syscall_frame* regs) {
    int       how    = (int)regs->ebx;
    uint32_t* set    = (uint32_t*)regs->ecx;
    uint32_t* oldset = (uint32_t*)regs->edx;

    if (!current_task) return -1;
    if (set    && !validate_user_ptr(set,    sizeof(uint32_t))) return -1;
    if (oldset && !validate_user_ptr(oldset, sizeof(uint32_t))) return -1;

    return task_sigprocmask(current_task, how, set, oldset);
}

// sigreturn() — restore user context from the signal frame on the stack
static int sys_sigreturn_impl(struct syscall_frame* regs) {
    if (!current_task || current_task->is_kernel) return -1;

    uint32_t user_esp = regs->useresp;
    if (user_esp < USER_SPACE_START || user_esp >= KERNEL_BASE) return -1;

    uint32_t page_off = user_esp & 0xFFFu;
    if (page_off + sizeof(signal_frame_t) > PAGE_SIZE) return -1;

    signal_frame_t frame_buf;

    // Read frame with double-check of page tables to prevent TOCTOU
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t* pd = current_task->page_directory;
        if (!pd) return -1;
        uint32_t pdi = PD_INDEX(user_esp);
        uint32_t pti = PT_INDEX(user_esp);
        if (!(pd[pdi] & PAGE_PRESENT)) return -1;

        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT)) return -1;

        uint32_t phys = pt[pti] & ~0xFFFu;

        memory_copy(&frame_buf, (signal_frame_t*)(phys + page_off), sizeof(frame_buf));

        // Re-validate: PTE must still point to the same physical page
        if ((pt[pti] & ~0xFFFu) == phys && (pt[pti] & PAGE_PRESENT))
            break;

        if (attempt == 1) return -1;
    }

    regs->eax     = frame_buf.eax;
    regs->ecx     = frame_buf.ecx;
    regs->edx     = frame_buf.edx;
    regs->ebx     = frame_buf.ebx;
    regs->ebp     = frame_buf.ebp;
    regs->esi     = frame_buf.esi;
    regs->edi     = frame_buf.edi;
    regs->eip     = frame_buf.eip;
    regs->eflags  = frame_buf.eflags;
    regs->useresp = frame_buf.esp;

    return 0;
}

// sigpending() — get the set of pending signals
static int sys_sigpending_impl(struct syscall_frame* regs) {
    uint32_t* set = (uint32_t*)regs->ebx;

    if (!current_task) return -1;
    if (!validate_user_ptr(set, sizeof(uint32_t))) return -1;

    *set = current_task->proc->pending_signals & current_task->proc->signal_mask;
    return 0;
}

// sigsuspend() — wait for a signal with a temporary mask
static int sys_sigsuspend_impl(struct syscall_frame* regs) {
    uint32_t* mask = (uint32_t*)regs->ebx;

    if (!current_task || current_task->is_kernel) return -1;
    if (!validate_user_ptr(mask, sizeof(uint32_t))) return -1;

    uint32_t sigmask = *mask;
    current_task->proc->saved_signal_mask = current_task->proc->signal_mask;
    current_task->proc->signal_mask = sigmask & ~SIG_UNCATCHABLE;
    current_task->proc->in_sigsuspend = 1;
    current_task->state = TASK_SLEEPING;

    schedule();   // will be woken by a delivered signal
    return -1;
}

// alarm() — schedule an SIGALRM after a given number of seconds
static int sys_alarm_impl(struct syscall_frame* regs) {
    uint32_t seconds = (uint32_t)regs->ebx;

    if (!current_task) return -1;

    uint32_t now = timer_ticks_get();
    uint32_t remaining = 0;

    // Return remaining time of the previous alarm, if any
    if (current_task->proc->alarm_ticks) {
        uint32_t left_ticks = (current_task->proc->alarm_ticks > now)
                              ? (current_task->proc->alarm_ticks - now) : 0;
        remaining = (left_ticks + TIMER_HZ_SIGNALS - 1) / TIMER_HZ_SIGNALS;
    }

    if (seconds == 0) {
        current_task->proc->alarm_ticks = 0;   // cancel
    } else {
        current_task->proc->alarm_ticks = now + seconds * TIMER_HZ_SIGNALS;
    }

    return (int)remaining;
}

// setitimer() — set an interval timer (ITIMER_REAL only)
static int sys_setitimer_impl(struct syscall_frame* regs) {
    int which                    = (int)regs->ebx;
    struct itimerval_k* newval   = (struct itimerval_k*)regs->ecx;
    struct itimerval_k* oldval   = (struct itimerval_k*)regs->edx;

    if (!current_task) return -1;
    if (which != 0) return -1;   // only ITIMER_REAL supported

    struct itimerval_k newval_buf;
    if (newval) {
        if (!validate_user_ptr(newval, sizeof(struct itimerval_k))) return -1;
        if (copy_from_user(&newval_buf, newval, sizeof(newval_buf)) != 0) return -1;
        newval = &newval_buf;
    }
    if (oldval && !validate_user_ptr(oldval, sizeof(struct itimerval_k))) return -1;

    uint32_t now = timer_ticks_get();

    // Return old value if requested
    if (oldval) {
        if (current_task->proc->itimer_value && current_task->proc->itimer_value > now) {
            uint32_t left = current_task->proc->itimer_value - now;
            oldval->it_value.tv_sec  = left / TIMER_HZ_SIGNALS;
            oldval->it_value.tv_usec = (long)((left % TIMER_HZ_SIGNALS) *
                                               (1000000 / TIMER_HZ_SIGNALS));
        } else {
            oldval->it_value.tv_sec  = 0;
            oldval->it_value.tv_usec = 0;
        }
        oldval->it_interval.tv_sec  = current_task->proc->itimer_interval / TIMER_HZ_SIGNALS;
        oldval->it_interval.tv_usec = (long)((current_task->proc->itimer_interval % TIMER_HZ_SIGNALS) *
                                              (1000000 / TIMER_HZ_SIGNALS));
    }

    // Set new value
    if (newval) {
        if (newval->it_value.tv_sec < 0 ||
            newval->it_value.tv_usec < 0 ||
            newval->it_interval.tv_sec < 0 ||
            newval->it_interval.tv_usec < 0)
            return -1;

        if ((uint64_t)newval->it_value.tv_sec * TIMER_HZ_SIGNALS > UINT32_MAX ||
            (uint64_t)newval->it_value.tv_usec * TIMER_HZ_SIGNALS > UINT32_MAX ||
            (uint64_t)newval->it_interval.tv_sec * TIMER_HZ_SIGNALS > UINT32_MAX ||
            (uint64_t)newval->it_interval.tv_usec * TIMER_HZ_SIGNALS > UINT32_MAX)
            return -1;

        uint32_t val_ticks = (uint32_t)(newval->it_value.tv_sec * TIMER_HZ_SIGNALS) +
                             (uint32_t)((newval->it_value.tv_usec * TIMER_HZ_SIGNALS) / 1000000);
        uint32_t int_ticks = (uint32_t)(newval->it_interval.tv_sec * TIMER_HZ_SIGNALS) +
                             (uint32_t)((newval->it_interval.tv_usec * TIMER_HZ_SIGNALS) / 1000000);

        if (val_ticks == 0) {
            current_task->proc->itimer_value    = 0;
            current_task->proc->itimer_interval = 0;
        } else {
            current_task->proc->itimer_value    = now + val_ticks;
            current_task->proc->itimer_interval = int_ticks;
        }
    }

    return 0;
}

// Public wrappers — called from syscall_table
int sys_kill(uint32_t pid)                     { return sys_kill_impl(pid); }
int sys_signal(uint32_t pid, uint32_t signal)  { return sys_signal_impl(pid, signal); }
int sys_sigaction(struct syscall_frame* regs)   { return sys_sigaction_impl(regs); }
int sys_sigprocmask(struct syscall_frame* regs) { return sys_sigprocmask_impl(regs); }
int sys_sigreturn(struct syscall_frame* regs)   { return sys_sigreturn_impl(regs); }
int sys_sigpending(struct syscall_frame* regs)  { return sys_sigpending_impl(regs); }
int sys_sigsuspend(struct syscall_frame* regs)  { return sys_sigsuspend_impl(regs); }
int sys_alarm(struct syscall_frame* regs)       { return sys_alarm_impl(regs); }
int sys_setitimer(struct syscall_frame* regs)   { return sys_setitimer_impl(regs); }