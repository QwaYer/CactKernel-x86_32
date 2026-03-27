#include "fcntl.h"
#include "syscall.h"
#include <stdarg.h>

int open(const char *pathname, int flags, ...) {
    return syscall(SYS_OPEN, (int)pathname, flags, 0);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    return syscall(SYS_FCNTL, fd, cmd, arg);
}
