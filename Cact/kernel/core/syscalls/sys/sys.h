#ifndef SC_SYS_H
#define SC_SYS_H

#include "kernel.h"
#include "task.h"
#include "vfs.h"

// Reboot command codes
#define REBOOT_RESTART  0x01234567u
#define REBOOT_HALT     0xCDEF0123u
#define REBOOT_POWEROFF 0x4321FEDCu

// uname() result structure
struct utsname_k {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

// System-level syscalls
int sys_print(char* msg);
int sys_mount(char* src, char* target, char* fstype);
int sys_umount(char* target);
int sys_reboot(uint32_t cmd);
int sys_uname(struct utsname_k* buf);

#endif 