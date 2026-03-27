#include "signal.h"
#include "syscall.h"

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return syscall(SYS_SIGPROCMASK, how, (int)set, (int)oldset);
}

int sigpending(sigset_t *set) {
    return syscall(SYS_SIGPENDING, (int)set, 0, 0);
}

int sigsuspend(const sigset_t *mask) {
    return syscall(SYS_SIGSUSPEND, (int)mask, 0, 0);
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)syscall(SYS_ALARM, (int)seconds, 0, 0);
}

int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value) {
    return syscall(SYS_SETITIMER, which, (int)new_value, (int)old_value);
}
