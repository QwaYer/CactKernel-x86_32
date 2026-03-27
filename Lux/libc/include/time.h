#ifndef _TIME_H
#define _TIME_H

typedef long time_t;

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int gettimeofday(struct timeval *tv, void *tz);
int clock_gettime(int clkid, struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
