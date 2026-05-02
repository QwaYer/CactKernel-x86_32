#ifndef SC_PROC_SIGNAL_H
#define SC_PROC_SIGNAL_H

#include "kernel.h"
#include "task.h"
#include "mod.h"

// Signal constants (if not already defined in task.h)
#ifndef SIG_DFL
#define SIG_DFL 0u
#endif
#ifndef SIG_IGN
#define SIG_IGN 1u
#endif
#ifndef SIG_UNCATCHABLE
#define SIG_UNCATCHABLE 0x00000003u  // SIGKILL | SIGSTOP
#endif
#ifndef NSIG
#define NSIG 32
#endif

// itimerval structure for setitimer()
struct itimerval_k {
    struct { long tv_sec; long tv_usec; } it_interval;
    struct { long tv_sec; long tv_usec; } it_value;
};

// Signal syscalls
int sys_kill(uint32_t pid);
int sys_signal(uint32_t pid, uint32_t signal);
int sys_sigaction(struct syscall_frame* regs);
int sys_sigprocmask(struct syscall_frame* regs);
int sys_sigreturn(struct syscall_frame* regs);
int sys_sigpending(struct syscall_frame* regs);
int sys_sigsuspend(struct syscall_frame* regs);
int sys_alarm(struct syscall_frame* regs);
int sys_setitimer(struct syscall_frame* regs);

#endif 