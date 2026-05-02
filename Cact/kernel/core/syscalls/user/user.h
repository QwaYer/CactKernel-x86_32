#ifndef SC_USER_H
#define SC_USER_H

#include "kernel.h"
#include "task.h"

// User/group identity syscalls
int sys_getuid(void);
int sys_getgid(void);
int sys_geteuid(void);
int sys_getegid(void);
int sys_setuid(uint32_t uid);
int sys_setgid(uint32_t gid);

#endif 