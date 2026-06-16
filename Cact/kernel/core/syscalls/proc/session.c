#include "session.h"

#define EPERM 1
#define ESRCH 3

// setsid() — create a new session; the calling process becomes session leader
int sys_setsid(void) {
    if (!current_task) return -1;
    current_task->proc->sid  = current_task->pid;
    current_task->proc->pgid = current_task->pid;
    return (int)current_task->proc->sid;
}

// setpgid() — set the process group ID of a process
int sys_setpgid(uint32_t pid, uint32_t pgid) {
    if (!current_task) return -1;
    struct task_struct* t;

    // pid == 0 means the calling process
    if (pid == 0 || pid == current_task->pid) {
        t = current_task;
    } else {
        // Only allow setting pgid of own child (simplified: same session)
        if (current_task->proc->euid != 0) return -EPERM;
        t = 0;
        struct task_struct* cur = task_list_head;
        while (cur) {
            if (cur->pid == pid) { t = cur; break; }
            cur = cur->next;
        }
        if (!t) return -ESRCH;
    }

    // pgid == 0 means use the target's own PID
    if (pgid == 0) pgid = t->pid;
    t->proc->pgid = pgid;
    return 0;
}

// getpgid() — get the process group ID of a process
int sys_getpgid(uint32_t pid) {
    if (!current_task) return -1;
    if (pid == 0) return (int)current_task->proc->pgid;

    struct task_struct* cur = task_list_head;
    while (cur) {
        if (cur->pid == pid) return (int)cur->proc->pgid;
        cur = cur->next;
    }
    return -1;
}

// getpgrp() — get the process group ID of the calling process
int sys_getpgrp(void) {
    if (!current_task) return -1;
    return (int)current_task->proc->pgid;
}