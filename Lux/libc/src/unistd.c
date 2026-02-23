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
