#include "sys.h"
#include "validate.h"
#include "resolve.h"
#include "helper.h"

int sys_print(char* msg) {
    if (!validate_user_str(msg)) return -1;
    kprint(msg);
    return 0;
}

int sys_mount(char* src, char* target, char* fstype) {
    (void)fstype;
    if (!validate_user_str(src))    return -1;
    if (!validate_user_str(target)) return -1;
    if (!current_task) return -1;
    if (current_task->euid != 0) return -1;
    vfs_node_t* src_node = _resolve_path(src);
    if (!src_node) return -1;
    char basename[128];
    vfs_node_t* parent = _resolve_parent(target, basename, 128);
    if (!parent || !basename[0]) return -1;
    return vfs_mount(parent, basename, src_node);
}

int sys_umount(char* target) {
    if (!validate_user_str(target)) return -1;
    if (!current_task) return -1;
    if (current_task->euid != 0) return -1;
    char basename[128];
    vfs_node_t* parent = _resolve_parent(target, basename, 128);
    if (!parent || !basename[0]) return -1;
    return vfs_umount(parent, basename);
}

int sys_reboot(uint32_t cmd) {
    if (!current_task) return -1;
    if (current_task->euid != 0) return -1;

    __asm__ volatile ("cli");

    if (cmd == REBOOT_POWEROFF) {
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    }

    uint8_t tmp;
    do {
        __asm__ volatile ("inb %1, %0" : "=a"(tmp) : "Nd"((uint16_t)0x64));
    } while (tmp & 0x02);
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    for (;;) __asm__ volatile ("hlt");
    return 0;
}

int sys_uname(struct utsname_k* buf) {
    if (!validate_user_ptr(buf, sizeof(struct utsname_k))) return -1;
    _kstrcpy(buf->sysname,  "CactOS",  65);
    _kstrcpy(buf->nodename, "cact",    65);
    _kstrcpy(buf->release,  "0.1.0",   65);
    _kstrcpy(buf->version,  "#1",      65);
    _kstrcpy(buf->machine,  "i686",    65);
    return 0;
}
