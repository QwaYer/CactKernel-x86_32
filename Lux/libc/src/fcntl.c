#include "fcntl.h"
#include "syscall.h"
#include <stdarg.h>

int open(const char *pathname, int flags, ...) {
    return syscall(SYS_OPEN, (int)pathname, flags, 0);
}
