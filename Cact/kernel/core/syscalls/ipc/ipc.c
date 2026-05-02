#include "ipc.h"
#include "validate.h"

// shmget() — get or create a shared memory segment
int sys_shmget(struct syscall_frame* regs) {
    int      key   = (int)regs->ebx;
    uint32_t size  = regs->ecx;
    int      flags = (int)regs->edx;
    return shm_get(key, size, flags);
}

// shmat() — attach a shared memory segment to the process address space
int sys_shmat(struct syscall_frame* regs) {
    int      shmid   = (int)regs->ebx;
    uint32_t shmaddr = regs->ecx;
    int      flags   = (int)regs->edx;
    return (int)shm_at(shmid, shmaddr, flags);
}

// shmdt() — detach a shared memory segment
int sys_shmdt(struct syscall_frame* regs) {
    uint32_t shmaddr = regs->ebx;
    return shm_dt(shmaddr);
}

// shmctl() — control operations on a shared memory segment
int sys_shmctl(struct syscall_frame* regs) {
    int   shmid = (int)regs->ebx;
    int   cmd   = (int)regs->ecx;
    void* buf   = (void*)regs->edx;
    if (buf && !validate_user_ptr(buf, 64)) return -1;
    return shm_ctl(shmid, cmd, buf);
}