#include "unistd.h"
#include "syscall.h"
#include "select.h"
#include "poll.h"

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

pid_t getppid(void) {
    return syscall(SYS_GETPPID, 0, 0, 0);
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

char *getcwd(char *buf, int size) {
    int ret = syscall(SYS_GETCWD, (int)buf, size, 0);
    if (ret < 0) return 0;
    return buf;
}

int chdir(const char *path) {
    return syscall(SYS_CHDIR, (int)path, 0, 0);
}

int ioctl(int fd, unsigned long cmd, void *arg) {
    return syscall(SYS_IOCTL, fd, (int)cmd, (int)arg);
}

int mkdir(const char *pathname, int mode) {
    (void)mode;
    return syscall(SYS_MKDIR, (int)pathname, 0, 0);
}

int rmdir(const char *pathname) {
    return syscall(SYS_RMDIR, (int)pathname, 0, 0);
}

int pipe(int pipefd[2]) {
    return syscall(SYS_PIPE, (int)pipefd, 0, 0);
}

int dup2(int oldfd, int newfd) {
    return syscall(SYS_DUP2, oldfd, newfd, 0);
}

/* select: pack 5 args into a struct, pass pointer in ebx */
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    select_args_t args;
    args.nfds     = nfds;
    args.readfds  = readfds;
    args.writefds = writefds;
    args.exceptfds = exceptfds;
    args.timeout  = timeout;
    return __syscall1(SYS_SELECT, (int)&args);
}

/* poll: 3 args fit directly in ebx/ecx/edx */
int poll(struct pollfd *fds, int nfds, int timeout_ms) {
    return __syscall3(SYS_POLL, (int)fds, nfds, timeout_ms);
}