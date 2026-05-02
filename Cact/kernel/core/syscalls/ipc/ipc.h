#ifndef SC_IPC_H
#define SC_IPC_H

#include "kernel.h"
#include "shm.h"
#include "mod.h"

// System V shared memory syscalls
int sys_shmget(struct syscall_frame* regs);
int sys_shmat(struct syscall_frame* regs);
int sys_shmdt(struct syscall_frame* regs);
int sys_shmctl(struct syscall_frame* regs);

#endif 