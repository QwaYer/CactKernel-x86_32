#include "user.h"

// getuid() — get the real user ID
int sys_getuid(void) { return current_task ? (int)current_task->proc->uid  : -1; }

// getgid() — get the real group ID
int sys_getgid(void) { return current_task ? (int)current_task->proc->gid  : -1; }

// geteuid() — get the effective user ID
int sys_geteuid(void) { return current_task ? (int)current_task->proc->euid : -1; }

// getegid() — get the effective group ID
int sys_getegid(void) { return current_task ? (int)current_task->proc->egid : -1; }

// setuid() — set the user ID (root may set any; others may only set to their own uid)
int sys_setuid(uint32_t uid) {
    if (!current_task) return -1;
    // Non-root can only set uid to their existing uid
    if (current_task->proc->euid != 0 && uid != current_task->proc->uid)
        return -1;
    current_task->proc->uid  = uid;
    current_task->proc->euid = uid;
    return 0;
}

// setgid() — set the group ID (root may set any; others may only set to their own gid)
int sys_setgid(uint32_t gid) {
    if (!current_task) return -1;
    // Non-root can only set gid to their existing gid
    if (current_task->proc->euid != 0 && gid != current_task->proc->gid)
        return -1;
    current_task->proc->gid  = gid;
    current_task->proc->egid = gid;
    return 0;
}