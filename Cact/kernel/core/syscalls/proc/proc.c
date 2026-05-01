#include "proc.h"
#include "validate.h"
#include "resolve.h"
#include "vfs.h"

#define EXEC_VALIDATE_MAX 256

int sys_get_pid(void) {
    return (int)current_task->pid;
}

int sys_getppid(void) {
    if (!current_task) return -1;
    return (int)current_task->parent_pid;
}

int sys_fork(struct syscall_frame* regs) {
    char hbuf[12];
    kprint("[SYSFORK] eip=0x");
    hex_to_ascii(regs->eip, hbuf); kprint(hbuf);
    kprint(" uesp=0x");
    hex_to_ascii(regs->useresp, hbuf); kprint(hbuf);
    kprint(" cs=0x");
    hex_to_ascii(regs->cs, hbuf); kprint(hbuf);
    kprint(" ss=0x");
    hex_to_ascii(regs->ss, hbuf); kprint(hbuf);
    kprint(" eflags=0x");
    hex_to_ascii(regs->eflags, hbuf); kprint(hbuf);
    kprint("\n");

    struct context_frame cf;
    cf.es        = regs->es;        cf.ds       = regs->ds;
    cf.edi       = regs->edi;       cf.esi      = regs->esi;
    cf.ebp       = regs->ebp;       cf.esp_dummy= regs->esp_dummy;
    cf.ebx       = regs->ebx;       cf.edx      = regs->edx;
    cf.ecx       = regs->ecx;       cf.eax      = regs->eax;
    cf.eip       = regs->eip;       cf.cs       = regs->cs;
    cf.eflags    = regs->eflags;
    cf.useresp   = regs->useresp;   cf.ss       = regs->ss;
    struct task_struct* child = task_fork(&cf);
    if (!child) {
        kprint("[SYSFORK] task_fork returned NULL!\n");
        return -1;
    }
    kprint("[SYSFORK] ok pid=");
    itoa((int)child->pid, hbuf); kprint(hbuf);
    kprint("\n");
    return (int)child->pid;
}

int sys_exec(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;
    if (!validate_user_str(path)) return -1;

    char** argv = (char**)regs->ecx;
    char** envp = (char**)regs->edx;

    if (argv) {
        if (!validate_user_ptr(argv, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&argv[i] < USER_SPACE_START || (uint32_t)&argv[i] >= KERNEL_BASE) return -1;
            if (!argv[i]) break;
            if (!validate_user_str(argv[i])) return -1;
        }
    }

    if (envp) {
        if (!validate_user_ptr(envp, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&envp[i] < USER_SPACE_START || (uint32_t)&envp[i] >= KERNEL_BASE) return -1;
            if (!envp[i]) break;
            if (!validate_user_str(envp[i])) return -1;
        }
    }

    {
        vfs_node_t* exec_node = _resolve_path(path);
        if (exec_node && vfs_check_perm(exec_node, VFS_PERM_EXEC) < 0)
            return -1;
    }

    struct context_frame cf;
    cf.eip     = regs->eip;
    cf.cs      = regs->cs;
    cf.eflags  = regs->eflags;
    cf.useresp = regs->useresp;
    cf.ss      = regs->ss;
    return task_exec(path, argv, envp, &cf);
}

int sys_exit(struct syscall_frame* regs) {
    if (!current_task) return -1;

    if (terminal_fg_pid == current_task->pid)
        terminal_fg_pid = 0;

    sched_task_exit((int)regs->ebx);

    /* Zombie: spin until timer interrupt preempts us */
    for (;;)
        __asm__ volatile ("hlt");
}

int sys_waitpid(struct syscall_frame* regs) {
    int  target_pid = (int)regs->ebx;
    int* status     = (int*)regs->ecx;

    if (!current_task) return -1;
    if (status && !validate_user_ptr(status, sizeof(int))) return -1;

    return sched_waitpid(target_pid, status);
}

int sys_sleep(struct syscall_frame* regs) {
    uint32_t ms = regs->ebx;
    if (!current_task) return -1;
    if (ms == 0) return 0;

    uint32_t ticks = (ms + 9) / 10;
    sched_sleep_ticks(ticks);
    return 0;
}
