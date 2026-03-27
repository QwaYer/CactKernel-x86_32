#ifndef _SYSCALL_H
#define _SYSCALL_H


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


#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2


static inline int __syscall0(int num) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline int __syscall1(int num, int a1) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1)
        : "memory"
    );
    return ret;
}

static inline int __syscall2(int num, int a1, int a2) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2)
        : "memory"
    );
    return ret;
}

static inline int __syscall3(int num, int a1, int a2, int a3) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

#endif