#include "unistd.h"
#include "syscall.h"

ssize_t read(int fd, void *buf, size_t count) {
    return syscall(SYS_READ, fd, (int)buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return syscall(SYS_WRITE, fd, (int)buf, count);
}

int close(int fd) {
    return syscall(SYS_CLOSE, fd, 0, 0);
}

pid_t fork(void) {
    return syscall(SYS_FORK, 0, 0, 0);
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    return syscall(SYS_EXEC, (int)pathname, (int)argv, (int)envp);
}

pid_t getpid(void) {
    return syscall(SYS_GETPID, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall(SYS_LSEEK, fd, offset, whence);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)options;
    return (pid_t)syscall(SYS_WAITPID, pid, (int)status, 0);
}

unsigned int sleep(unsigned int seconds) {
    syscall(SYS_SLEEP, (int)(seconds * 1000), 0, 0);
    return 0;
}

int usleep(unsigned int usec) {
    unsigned int ms = (usec + 999) / 1000;
    if (ms == 0) ms = 1;
    return syscall(SYS_SLEEP, (int)ms, 0, 0);
}

void *sbrk(int increment) {
    int cur = syscall(SYS_BRK, 0, 0, 0);
    if (cur < 0) return (void*)-1;
    if (increment == 0) return (void*)cur;
    int new_brk = syscall(SYS_BRK, cur + increment, 0, 0);
    if (new_brk < 0) return (void*)-1;
    return (void*)cur;
}

int brk(void *addr) {
    int ret = syscall(SYS_BRK, (int)addr, 0, 0);
    return (ret < 0) ? -1 : 0;
}