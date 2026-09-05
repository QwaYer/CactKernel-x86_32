#ifndef SYSCALLS_H
#define SYSCALLS_H

// Final minimal syscall ABI (15 entries).
//
// Everything else is served through VFS nodes (ioctl_abi.h): identity/process
// info via /proc/self, process control via /proc/self/ctl, filesystem and
// fd-level operations via FDCTL_*/DIRCTL_* ioctls, network via /dev/net and
// SOCKCTL_*, system ops via /dev/sys, pipes via /dev/pipe, time via
// /proc/time, uname via /proc/uname.
//
// User convention (see syscall_entries.asm):
//   EAX = number, EBX = arg1, ESI = arg2, EDI = arg3 (remapped to ECX/EDX).

typedef enum {
    SYS_OPEN      = 0,
    SYS_CLOSE     = 1,
    SYS_READ      = 2,
    SYS_WRITE     = 3,
    SYS_IOCTL     = 4,
    SYS_POLL      = 5,
    SYS_FORK      = 6,
    SYS_EXEC      = 7,
    SYS_EXIT      = 8,
    SYS_WAITPID   = 9,
    SYS_BRK       = 10,
    SYS_MMAP      = 11,
    SYS_MUNMAP    = 12,
    SYS_MPROTECT  = 13,
    SYS_SIGRETURN = 14,
    SYSCALL_COUNT = 15,
} syscall_num_t;

#endif
