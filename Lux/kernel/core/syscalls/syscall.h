#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PRINT   0
#define SYS_GETPID  1
#define SYS_OPEN    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_CREATE  5
#define SYS_DELETE  6
#define SYS_EXIT    7
#define SYS_CLOSE   8
#define SYS_FORK    9
#define SYS_EXEC    10
#define SYS_KILL    11
#define SYS_SIGNAL  12
#define SYS_MMAP    13
#define SYS_MUNMAP  14
#define SYS_MPROTECT 15
#define SYS_SIGRETURN 16
#define SYS_SIGACTION 17
#define SYS_PIPE    18
#define SYS_DUP2    19
#define SYS_LSEEK   20
#define SYS_WAITPID 21
#define SYS_SLEEP   22
#define SYS_BRK     23
#define SYS_GETCWD  24
#define SYS_CHDIR   25
#define SYS_STAT    26
#define SYS_FSTAT   27
#define SYS_GETPPID 28
#define SYS_GETDENTS 29
#define SYS_RENAME   30
#define SYS_IOCTL    31
#define SYS_MKDIR    32
#define SYS_RMDIR    33
#define SYS_FCNTL         34
#define SYS_GETTIMEOFDAY  35
#define SYS_CLOCK_GETTIME 36
#define SYS_NANOSLEEP     37
#define SYS_SIGPROCMASK   38
#define SYS_SIGPENDING    39
#define SYS_SIGSUSPEND    40
#define SYS_ALARM         41
#define SYS_SETITIMER     42

/* Socket syscalls */
#define SYS_SOCKET        43
#define SYS_BIND          44
#define SYS_CONNECT       45
#define SYS_LISTEN        46
#define SYS_ACCEPT        47
#define SYS_SEND          48
#define SYS_RECV          49
#define SYS_SENDTO        50
#define SYS_RECVFROM      51

/* VFS symlink / hard-link syscalls */
#define SYS_SYMLINK       54
#define SYS_READLINK      55
#define SYS_LINK          56
#define SYS_UNLINK        57

/* I/O multiplexing */
#define SYS_SELECT        58
#define SYS_POLL          59

int syscall(int num, int p1, int p2, int p3);

#endif