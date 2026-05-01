#ifndef SC_TIME_H
#define SC_TIME_H

#include "kernel.h"
#include "mod.h"

#define TIMER_HZ         100
#define TIMER_HZ_SIGNALS 100

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

int sys_gettimeofday(struct syscall_frame* regs);
int sys_clock_gettime(struct syscall_frame* regs);
int sys_nanosleep(struct syscall_frame* regs);

#endif /* SC_TIME_H */
