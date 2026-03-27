#include "time.h"
#include "syscall.h"

int gettimeofday(struct timeval *tv, void *tz) {
    return __syscall2(SYS_GETTIMEOFDAY, (int)tv, (int)tz);
}

int clock_gettime(int clkid, struct timespec *tp) {
    return __syscall2(SYS_CLOCK_GETTIME, clkid, (int)tp);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return __syscall2(SYS_NANOSLEEP, (int)req, (int)rem);
}
