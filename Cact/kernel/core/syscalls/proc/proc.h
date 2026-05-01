#ifndef SC_PROC_PROC_H
#define SC_PROC_PROC_H

#include "kernel.h"
#include "task.h"
#include "mod.h"

extern void sched_sleep_ticks(uint32_t ticks);
extern void sched_task_exit(int exit_code);
extern int  sched_waitpid(int target_pid, int* status);

int sys_get_pid(void);
int sys_getppid(void);
int sys_fork(struct syscall_frame* regs);
int sys_exec(struct syscall_frame* regs);
int sys_exit(struct syscall_frame* regs);
int sys_waitpid(struct syscall_frame* regs);
int sys_sleep(struct syscall_frame* regs);

#endif /* SC_PROC_PROC_H */
