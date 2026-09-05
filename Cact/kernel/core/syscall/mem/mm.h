#ifndef SC_MM_H
#define SC_MM_H

#include "kernel.h"
#include "task.h"
#include "memory.h"
#include "mmap.h"
#include "mod.h"

// mmap protection flags
#ifndef PROT_READ
#define PROT_READ  0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif

// mmap mapping flags
#ifndef MAP_SHARED
#define MAP_SHARED  0x01
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_FIXED
#define MAP_FIXED   0x10
#endif
#ifndef MAP_ANON
#define MAP_ANON    0x20
#endif
#ifndef MAP_FAILED
#define MAP_FAILED  ((void*)-1)
#endif

// Memory management syscalls
int sys_brk(struct syscall_frame* regs);
int sys_mmap(struct syscall_frame* regs);
int sys_munmap(struct syscall_frame* regs);
int sys_mprotect(struct syscall_frame* regs);

#endif 