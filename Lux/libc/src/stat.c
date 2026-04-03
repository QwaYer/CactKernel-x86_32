#include "stat.h"
#include "syscall.h"

int stat(const char *path, struct stat *buf) {
    return syscall(SYS_STAT, (int)path, (int)buf, 0);
}

int fstat(int fd, struct stat *buf) {
    return syscall(SYS_FSTAT, fd, (int)buf, 0);
}