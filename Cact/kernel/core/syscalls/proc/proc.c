#include "proc.h"
#include "validate.h"
#include "resolve.h"
#include "vfs.h"

// Maximum number of argv/envp entries to validate (prevents runaway loops)
#define EXEC_VALIDATE_MAX 256

// getpid() — return the calling process's PID
int sys_get_pid(void) {
    return (int)current_task->pid;
}

// getppid() — return the parent process's PID
int sys_getppid(void) {
    if (!current_task) return -1;
    return (int)current_task->proc->parent_pid;
}

// fork() — create a child process that is a copy of the caller
int sys_fork(struct syscall_frame* regs) {
    // Copy the syscall frame into a context_frame for task_fork
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
        return -1;
    }
    return (int)child->pid;   // child sees 0, parent sees child PID
}

// exec() — replace the current process image with a new program
int sys_exec(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;
    if (!validate_user_str(path)) return -1;

    char** argv = (char**)regs->ecx;
    char** envp = (char**)regs->edx;

    // Validate argv array and each string pointer
    if (argv) {
        if (!validate_user_ptr(argv, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&argv[i] < USER_SPACE_START || (uint32_t)&argv[i] >= KERNEL_BASE) return -1;
            if (!argv[i]) break;
            if (!validate_user_str(argv[i])) return -1;
        }
    }

    // Validate envp array and each string pointer
    if (envp) {
        if (!validate_user_ptr(envp, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&envp[i] < USER_SPACE_START || (uint32_t)&envp[i] >= KERNEL_BASE) return -1;
            if (!envp[i]) break;
            if (!validate_user_str(envp[i])) return -1;
        }
    }

    // Check execute permission on the file
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
    return task_exec(path, argv, envp, &cf);   // never returns on success
}

// exit() — terminate the calling process
int sys_exit(struct syscall_frame* regs) {
    if (!current_task) return -1;

    // Clear foreground process group if this was the terminal's leader
    if (terminal_fg_pid == current_task->pid)
        terminal_fg_pid = 0;

    sched_task_exit((int)regs->ebx);   // sets zombie state, reschedules

    // Safety net: if schedule() returns, spin forever in HLT
    for (;;)
        __asm__ volatile ("hlt");
}

// waitpid() — wait for a child process to change state
int sys_waitpid(struct syscall_frame* regs) {
    int  target_pid = (int)regs->ebx;
    int* status     = (int*)regs->ecx;

    if (!current_task) return -1;
    if (status && !validate_user_ptr(status, sizeof(int))) return -1;

    return sched_waitpid(target_pid, status);
}

// sleep() — sleep for a given number of milliseconds
int sys_sleep(struct syscall_frame* regs) {
    uint32_t ms = regs->ebx;
    if (!current_task) return -1;
    if (ms == 0) return 0;

    // Convert ms to timer ticks (100 Hz = 10 ms per tick)
    uint32_t ticks = (ms + 9) / 10;
    sched_sleep_ticks(ticks);
    return 0;
}