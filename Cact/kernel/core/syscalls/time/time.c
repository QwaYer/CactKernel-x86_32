#include "time.h"
#include "validate.h"
#include "helper.h"
#include "task.h"

int sys_gettimeofday(struct syscall_frame* regs) {
    struct timeval* tv = (struct timeval*)regs->ebx;

    if (!tv) return 0;
    if (!validate_user_ptr(tv, sizeof(struct timeval))) return -1;

    uint32_t ticks = timer_ticks_get();
    tv->tv_sec  = (long)(ticks / TIMER_HZ);
    tv->tv_usec = (long)((ticks % TIMER_HZ) * (1000000 / TIMER_HZ));
    return 0;
}

int sys_clock_gettime(struct syscall_frame* regs) {
    int clkid           = (int)regs->ebx;
    struct timespec* tp = (struct timespec*)regs->ecx;

    if (!tp) return -1;
    if (!validate_user_ptr(tp, sizeof(struct timespec))) return -1;
    if (clkid != CLOCK_REALTIME && clkid != CLOCK_MONOTONIC) return -1;

    uint32_t ticks = timer_ticks_get();
    tp->tv_sec  = (long)(ticks / TIMER_HZ);
    tp->tv_nsec = (long)((ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
    return 0;
}

int sys_nanosleep(struct syscall_frame* regs) {
    struct timespec* req = (struct timespec*)regs->ebx;
    struct timespec* rem = (struct timespec*)regs->ecx;

    if (!req) return -1;
    if (!validate_user_ptr(req, sizeof(struct timespec))) return -1;
    if (rem && !validate_user_ptr(rem, sizeof(struct timespec))) return -1;

    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000)
        return -1;

    if (!current_task) return -1;

    extern void sched_sleep_ticks(uint32_t ticks);

    uint32_t ms   = (uint32_t)(req->tv_sec * 1000) +
                    (uint32_t)((req->tv_nsec + 999999) / 1000000);
    uint32_t ticks = (ms + (1000 / TIMER_HZ) - 1) / (1000 / TIMER_HZ);

    if (ticks == 0) {
        if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
        return 0;
    }

    sched_sleep_ticks(ticks);

    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
