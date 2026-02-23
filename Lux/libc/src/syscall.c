#include "syscall.h"

int syscall(int num, int p1, int p2, int p3) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(p1), "c"(p2), "d"(p3)
        : "memory"
    );
    return ret;
}
