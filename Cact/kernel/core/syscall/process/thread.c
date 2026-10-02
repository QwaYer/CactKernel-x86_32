#include "task.h"
#include "validate.h"
#include "ioctl_abi.h"

/* User threads and futexes.
 *
 * The scheduler creates the thread itself (sched_create_thread, in
 * sched/src/task_thread.rs); this file provides the /proc/self/ctl entry points
 * and the futex wait/wake table that lets userspace (libc pthreads) build
 * mutexes, condition variables and joins.  A futex is keyed by the pair
 * (address space, user address), so two threads of one process rendezvous while
 * an unrelated process reusing the same virtual address does not.
 */

#define EINTR     4
#define EAGAIN    11
#define ENOMEM    12
#define EFAULT    14
#define EINVAL    22
#define ETIMEDOUT 110

#define FUTEX_OP_WAIT 0
#define FUTEX_OP_WAKE 1

#define FUTEX_MAX_WAITERS 256

typedef struct {
    uint32_t            pd;      /* address-space identity (its page directory) */
    uint32_t            uaddr;   /* futex word, user VA */
    struct task_struct* task;
} futex_waiter_t;

/* BSS is zeroed at boot, and a zeroed irq_spinlock_t is a valid unlocked lock,
 * so no explicit init is needed (and there is no early-boot hook here). */
static futex_waiter_t futex_waiters[FUTEX_MAX_WAITERS];
static irq_spinlock_t futex_lock;

extern uint32_t timer_ticks_get(void);
extern void sched_task_exit(int exit_code);

/* Block until *uaddr changes or the timeout expires.  Returns 0 on wake,
 * -EAGAIN if the value already differed, -ETIMEDOUT, or -EINTR. */
static int futex_do_wait(uint32_t uaddr, int32_t val, int32_t timeout_ms) {
    uint32_t* word = (uint32_t*)uaddr;
    if (!validate_user_ptr(word, sizeof(uint32_t))) return -EFAULT;
    if (!current_task || !current_task->proc) return -EINVAL;

    /* A pending signal must not be lost to the block: report -EINTR and let the
     * caller's syscall-return path deliver it. */
    if (task_signal_pending_current()) return -EINTR;

    uint32_t deadline = 0;
    if (timeout_ms > 0) {
        uint32_t ticks = ((uint32_t)timeout_ms + 9) / 10;
        if (ticks == 0) ticks = 1;
        deadline = timer_ticks_get() + ticks;
    }

    uint32_t pd = (uint32_t)current_task->page_directory;

    irq_spinlock_acquire(&futex_lock);

    /* Re-check under the table lock: a waker that changed *word before calling
     * WAKE either saw our entry (and removed it) or we see the new value here. */
    if ((int32_t)*word != val) {
        irq_spinlock_release(&futex_lock);
        return -EAGAIN;
    }

    int slot = -1;
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (!futex_waiters[i].task) { slot = i; break; }
    }
    if (slot < 0) {
        irq_spinlock_release(&futex_lock);
        return -ENOMEM;
    }
    futex_waiters[slot].pd    = pd;
    futex_waiters[slot].uaddr = uaddr;
    futex_waiters[slot].task  = current_task;

    current_task->proc->sleep_until = deadline;
    current_task->proc->intr_wait   = 1;   /* a signal may wake us */
    current_task->state = TASK_SLEEPING;
    irq_spinlock_release(&futex_lock);

    /* sched_park_prev() puts us on the blocked queue (deadline 0) or the timer
     * wheel (deadline != 0) once the switch commits. */
    schedule();

    current_task->proc->intr_wait = 0;

    /* Make sure we are not left in the table — but only if this slot still
     * holds *us*.  The waker cleared it already; if we were woken some other
     * way (signal/timeout) we must clear it ourselves.  An unconditional clear
     * here races another waiter that has since reused the same slot and would
     * silently drop its entry (a lost wake-up). */
    irq_spinlock_acquire(&futex_lock);
    if (futex_waiters[slot].task == current_task) {
        futex_waiters[slot].task  = 0;
        futex_waiters[slot].pd    = 0;
        futex_waiters[slot].uaddr = 0;
    }
    irq_spinlock_release(&futex_lock);

    if (deadline != 0 && timer_ticks_get() >= deadline) return -ETIMEDOUT;
    if (task_signal_pending_current()) return -EINTR;
    return 0;
}

/* Wake up to `count` waiters on *uaddr in address space `pd`; returns how many
 * were woken.  Also used by the scheduler's thread-exit path through
 * cact_futex_wake_addr(). */
static int futex_wake_pd(uint32_t pd, uint32_t uaddr, int32_t count) {
    if (count <= 0) count = 1;

    int woken = 0;

    irq_spinlock_acquire(&futex_lock);
    for (int i = 0; i < FUTEX_MAX_WAITERS && woken < count; i++) {
        if (futex_waiters[i].task &&
            futex_waiters[i].uaddr == uaddr &&
            futex_waiters[i].pd == pd) {
            struct task_struct* t = futex_waiters[i].task;
            futex_waiters[i].task  = 0;
            futex_waiters[i].pd    = 0;
            futex_waiters[i].uaddr = 0;
            /* mlfq_wake_task takes SCHEDULER_LOCK, so drop the table lock first
             * to keep a strict (futex before scheduler) lock order. */
            irq_spinlock_release(&futex_lock);
            mlfq_wake_task(t);
            irq_spinlock_acquire(&futex_lock);
            woken++;
        }
    }
    irq_spinlock_release(&futex_lock);

    return woken;
}

/* Kernel FFI: the scheduler clears a thread's join word and wakes its waiters
 * when the thread exits (ROADMAP: clear_child_tid). */
int cact_futex_wake_addr(uint32_t* pd, uint32_t* uaddr, int count) {
    return futex_wake_pd((uint32_t)pd, (uint32_t)uaddr, count);
}

/* Wake up to `count` waiters on *uaddr in the calling task's address space. */
static int futex_do_wake(uint32_t uaddr, int32_t count) {
    if (!current_task) return -EINVAL;
    return futex_wake_pd((uint32_t)current_task->page_directory, uaddr, count);
}

int proc_thread_create(const void* arg) {
    cact_thread_create_arg_t a;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (!a.entry || a.user_esp == 0) return -EINVAL;
    if ((uint32_t)a.entry < USER_SPACE_START || (uint32_t)a.entry >= KERNEL_BASE)
        return -EINVAL;
    if (a.user_esp < USER_SPACE_START || a.user_esp >= KERNEL_BASE)
        return -EINVAL;
    if (a.set_child_tid &&
        !validate_user_ptr((void*)a.set_child_tid, sizeof(uint32_t)))
        return -EINVAL;
    if (a.clear_child_tid &&
        !validate_user_ptr((void*)a.clear_child_tid, sizeof(uint32_t)))
        return -EINVAL;
    return sched_create_thread(a.entry, a.user_esp, a.tls,
                               a.set_child_tid, a.clear_child_tid);
}

int proc_thread_exit(uint32_t code) {
    sched_task_exit((int)code);   /* never returns */
    return 0;
}

/* Lightweight identity for pthread internals: avoids opening /proc/self/info on
 * every pthread_self()/join. */
int proc_thread_gettid(void) {
    return current_task ? (int)current_task->pid : -1;
}

int proc_futex(const void* arg) {
    cact_futex_arg_t a;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    switch (a.op) {
    case FUTEX_OP_WAIT:
        return futex_do_wait(a.uaddr, a.val, a.timeout_ms);
    case FUTEX_OP_WAKE:
        return futex_do_wake(a.uaddr, a.val);
    default:
        return -EINVAL;
    }
}
