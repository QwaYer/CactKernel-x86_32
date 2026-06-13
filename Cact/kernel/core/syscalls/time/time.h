#ifndef SC_TIME_H
#define SC_TIME_H

#include "kernel.h"
#include "mod.h"

// Timer frequency constants (250 Hz = 4 ms per tick)
#define TIMER_HZ         250
#define TIMER_HZ_SIGNALS 250

// Clock IDs for clock_gettime()
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

// Userspace time structures
struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

// Time syscalls
int sys_gettimeofday(struct syscall_frame* regs);
int sys_clock_gettime(struct syscall_frame* regs);
int sys_nanosleep(struct syscall_frame* regs);

#endif 