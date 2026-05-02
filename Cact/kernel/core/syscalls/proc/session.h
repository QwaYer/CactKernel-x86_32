#ifndef SC_PROC_SESSION_H
#define SC_PROC_SESSION_H

#include "kernel.h"
#include "task.h"

// Session and process group syscalls
int sys_setsid(void);
int sys_setpgid(uint32_t pid, uint32_t pgid);
int sys_getpgid(uint32_t pid);
int sys_getpgrp(void);

#endif 