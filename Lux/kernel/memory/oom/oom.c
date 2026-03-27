#include "oom.h"
#include "memory.h"
#include "kernel.h"
#include "task.h"
#include "libc.h"
#include "proc_mm.h"

static oom_stats_t g_stats;

/* Score a process for OOM selection.
 * Higher score = more likely to be killed.
 * Returns 0 for unkillable processes. */
static uint32_t oom_score(struct task_struct* t)
{
    if (!t) return 0;
    if (t->is_kernel) return 0;
    if (t->pid <= 1) return 0;              /* never kill idle or init/shell */
    if (t->state == TASK_ZOMBIE) return 0;   /* already dying */

    return t->mm.count;                      /* page count = score */
}

int oom_kill(void)
{
    extern struct task_struct* volatile task_list_head;
    extern irq_spinlock_t scheduler_lock;

    irq_spinlock_acquire(&scheduler_lock);

    if (!task_list_head) {
        irq_spinlock_release(&scheduler_lock);
        return -1;
    }

    struct task_struct* victim = 0;
    uint32_t best_score = 0;

    struct task_struct* t = task_list_head;
    int count = 0;
    do {
        uint32_t score = oom_score(t);
        if (score > best_score) {
            best_score = score;
            victim = t;
        }
        t = t->next;
        count++;
    } while (t != task_list_head && count < 256);

    if (!victim || best_score == 0) {
        irq_spinlock_release(&scheduler_lock);
        kprint_color("[OOM] no killable process found\n", COLOR_LIGHT_RED);
        return -1;
    }

    uint32_t victim_pid   = victim->pid;
    uint32_t victim_pages = victim->mm.count;

    /* Send SIGKILL */
    victim->pending_signals |= SIGKILL;

    /* Wake the victim if sleeping/waiting so it can be reaped */
    if (victim->state == TASK_SLEEPING) {
        extern sched_queue_t sleep_queue;
        sched_queue_remove(&sleep_queue, victim);
        victim->state = TASK_READY;
        sched_queue_push(&ready_queue, victim);
    }
    if (victim->state == TASK_WAITING) {
        extern sched_queue_t wait_queue;
        sched_queue_remove(&wait_queue, victim);
        victim->state = TASK_READY;
        sched_queue_push(&ready_queue, victim);
    }

    irq_spinlock_release(&scheduler_lock);

    g_stats.oom_kills++;
    g_stats.pages_reclaimed += victim_pages;
    g_stats.last_killed_pid  = victim_pid;

    /* Log the kill */
    char buf[16];
    kprint_color("\n[OOM] Killed pid=", COLOR_LIGHT_RED);
    itoa((int)victim_pid, buf);
    kprint_color(buf, COLOR_LIGHT_RED);
    kprint_color(" (", COLOR_LIGHT_RED);
    itoa((int)victim_pages, buf);
    kprint_color(buf, COLOR_LIGHT_RED);
    kprint_color(" pages)\n", COLOR_LIGHT_RED);

    /* Force reap so memory becomes available immediately */
    task_reap();

    return 0;
}

oom_stats_t oom_get_stats(void) { return g_stats; }

void oom_print_stats(void)
{
    char buf[16];
    kprint("[OOM] === OOM Killer Statistics ===\n");
    kprint("  oom_kills:       "); itoa((int)g_stats.oom_kills,       buf); kprint(buf); kprint("\n");
    kprint("  pages_reclaimed: "); itoa((int)g_stats.pages_reclaimed, buf); kprint(buf); kprint("\n");
    kprint("  last_killed_pid: "); itoa((int)g_stats.last_killed_pid, buf); kprint(buf); kprint("\n");
}
