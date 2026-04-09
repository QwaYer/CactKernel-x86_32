#include "oom.h"
#include "memory.h"
#include "kernel.h"
#include "task.h"
#include "klib.h"
#include "proc_mm.h"

static oom_stats_t g_stats;

static uint32_t oom_score(struct task_struct* t)
{
    if (!t) return 0;
    if (t->is_kernel) return 0;
    if (t->pid <= 1) return 0;            
    if (t->state == TASK_ZOMBIE) return 0; 

    return t->mm.count;                   
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
    while (t && count < 256) {
        uint32_t score = oom_score(t);
        if (score > best_score) {
            best_score = score;
            victim = t;
        }
        t = t->next;
        count++;
    }

    if (!victim || best_score == 0) {
        irq_spinlock_release(&scheduler_lock);
        kprint_color("[OOM] no killable process found\n", COLOR_LIGHT_RED);
        return -1;
    }

    uint32_t victim_pid   = victim->pid;
    uint32_t victim_pages = victim->mm.count;

    extern void task_signal_locked(uint32_t pid, uint32_t signal);
    task_signal_locked(victim_pid, SIGKILL);

    irq_spinlock_release(&scheduler_lock);

    g_stats.oom_kills++;
    g_stats.pages_reclaimed += victim_pages;
    g_stats.last_killed_pid  = victim_pid;

    char buf[16];
    kprint_color("\n[OOM] Killed pid=", COLOR_LIGHT_RED);
    itoa((int)victim_pid, buf);
    kprint_color(buf, COLOR_LIGHT_RED);
    kprint_color(" (", COLOR_LIGHT_RED);
    itoa((int)victim_pages, buf);
    kprint_color(buf, COLOR_LIGHT_RED);
    kprint_color(" pages)\n", COLOR_LIGHT_RED);

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
