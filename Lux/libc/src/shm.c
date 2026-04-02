#include "shm.h"
#include "syscall.h"

int shmget(int key, unsigned int size, int flags) {
    return syscall(SYS_SHMGET, key, (int)size, flags);
}

void* shmat(int shmid, const void* shmaddr, int flags) {
    int ret = syscall(SYS_SHMAT, shmid, (int)shmaddr, flags);
    if (ret == -1) return (void*)-1;
    return (void*)ret;
}

int shmdt(const void* shmaddr) {
    return syscall(SYS_SHMDT, (int)shmaddr, 0, 0);
}

int shmctl(int shmid, int cmd, void* buf) {
    return syscall(SYS_SHMCTL, shmid, cmd, (int)buf);
}