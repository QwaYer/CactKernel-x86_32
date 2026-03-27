#include "dirent.h"
#include "syscall.h"

int getdents(int fd, struct dirent *buf, unsigned int count) {
    return syscall(SYS_GETDENTS, fd, (int)buf, (int)count);
}
