#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stdint.h>
#include "time.h"

typedef uint32_t sigset_t;

/* how values for sigprocmask */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/*
 * Signal bitmasks — each signal occupies one bit.
 * These values are used with sigprocmask/sigpending/sigsuspend
 * and match the kernel's internal representation.
 */
#define SIGKILL  (1u << 0)
#define SIGTERM  (1u << 1)
#define SIGSTOP  (1u << 2)
#define SIGCONT  (1u << 3)
#define SIGPIPE  (1u << 4)
#define SIGALRM  (1u << 5)
#define SIGCHLD  (1u << 6)

/* itimer types for setitimer/getitimer */
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval {
    struct timeval it_interval; /* interval for periodic timer */
    struct timeval it_value;    /* time until next expiry      */
};

int          sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int          sigpending(sigset_t *set);
int          sigsuspend(const sigset_t *mask);
unsigned int alarm(unsigned int seconds);
int          setitimer(int which, const struct itimerval *new_value,
                       struct itimerval *old_value);

#endif /* _SIGNAL_H */
