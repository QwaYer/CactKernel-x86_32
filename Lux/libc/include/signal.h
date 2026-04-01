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
#define SIGFPE   (1u << 7)
#define SIGSEGV  (1u << 8)
#define SIGWINCH (1u << 9)
#define SIGHUP   (1u << 10)   /* hangup — terminal closed          */
#define SIGINT   (1u << 11)   /* interrupt — Ctrl-C from TTY       */
#define SIGQUIT  (1u << 12)   /* quit     — Ctrl-\ from TTY        */

/* ioctl requests for terminal window size */
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

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
