#include "user.h"

int sys_getuid()  { return current_task ? (int)current_task->uid  : -1; }
int sys_getgid()  { return current_task ? (int)current_task->gid  : -1; }
int sys_geteuid() { return current_task ? (int)current_task->euid : -1; }
int sys_getegid() { return current_task ? (int)current_task->egid : -1; }

int sys_setuid(uint32_t uid) {
    if (!current_task) return -1;
    if (current_task->euid != 0 && uid != current_task->uid)
        return -1;
    current_task->uid  = uid;
    current_task->euid = uid;
    return 0;
}

int sys_setgid(uint32_t gid) {
    if (!current_task) return -1;
    if (current_task->euid != 0 && gid != current_task->gid)
        return -1;
    current_task->gid  = gid;
    current_task->egid = gid;
    return 0;
}
