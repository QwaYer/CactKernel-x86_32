#include "user.h"

// getuid() — get the real user ID
int sys_getuid(void) { return current_task ? (int)current_task->uid  : -1; }

// getgid() — get the real group ID
int sys_getgid(void) { return current_task ? (int)current_task->gid  : -1; }

// geteuid() — get the effective user ID
int sys_geteuid(void) { return current_task ? (int)current_task->euid : -1; }

// getegid() — get the effective group ID
int sys_getegid(void) { return current_task ? (int)current_task->egid : -1; }

// setuid() — set the user ID (root may set any; others may only set to their own uid)
int sys_setuid(uint32_t uid) {
    if (!current_task) return -1;
    // Non-root can only set uid to their existing uid
    if (current_task->euid != 0 && uid != current_task->uid)
        return -1;
    current_task->uid  = uid;
    current_task->euid = uid;
    return 0;
}

// setgid() — set the group ID (root may set any; others may only set to their own gid)
int sys_setgid(uint32_t gid) {
    if (!current_task) return -1;
    // Non-root can only set gid to their existing gid
    if (current_task->euid != 0 && gid != current_task->gid)
        return -1;
    current_task->gid  = gid;
    current_task->egid = gid;
    return 0;
}