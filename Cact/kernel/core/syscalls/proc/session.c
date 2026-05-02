#include "session.h"

// setsid() — create a new session; the calling process becomes session leader
int sys_setsid(void) {
    if (!current_task) return -1;
    current_task->sid  = current_task->pid;
    current_task->pgid = current_task->pid;
    return (int)current_task->sid;
}

// setpgid() — set the process group ID of a process
int sys_setpgid(uint32_t pid, uint32_t pgid) {
    if (!current_task) return -1;
    struct task_struct* t;

    // pid == 0 means the calling process
    if (pid == 0 || pid == current_task->pid) {
        t = current_task;
    } else {
        t = 0;
        struct task_struct* cur = task_list_head;
        while (cur) {
            if (cur->pid == pid) { t = cur; break; }
            cur = cur->next;
        }
        if (!t) return -1;   // target process not found
    }

    // pgid == 0 means use the target's own PID
    if (pgid == 0) pgid = t->pid;
    t->pgid = pgid;
    return 0;
}

// getpgid() — get the process group ID of a process
int sys_getpgid(uint32_t pid) {
    if (!current_task) return -1;
    if (pid == 0) return (int)current_task->pgid;

    struct task_struct* cur = task_list_head;
    while (cur) {
        if (cur->pid == pid) return (int)cur->pgid;
        cur = cur->next;
    }
    return -1;
}

// getpgrp() — get the process group ID of the calling process
int sys_getpgrp(void) {
    if (!current_task) return -1;
    return (int)current_task->pgid;
}