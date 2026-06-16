#include "sys.h"
#include "validate.h"
#include "helper.h"

// print() — print a message to the kernel console (debug)
int sys_print(char* msg) {
    if (!validate_user_str(msg)) return -1;
    char buf[USER_STR_MAX];
    int len = 0;
    while (msg[len] && len < USER_STR_MAX - 1) { buf[len] = msg[len]; len++; }
    buf[len] = '\0';
    kprint(buf);
    return 0;
}

// mount() — mount a filesystem on a directory (root only)
int sys_mount(char* src, char* target, char* fstype) {
    (void)fstype;
    if (!validate_user_str(src))    return -1;
    if (!validate_user_str(target)) return -1;
    if (!current_task) return -1;
    if (current_task->proc->euid != 0) return -1;

    char src_buf[USER_STR_MAX];
    int slen = 0;
    while (src[slen] && slen < USER_STR_MAX - 1) { src_buf[slen] = src[slen]; slen++; }
    src_buf[slen] = '\0';
    if (slen == 0) return -1;

    char tgt_buf[USER_STR_MAX];
    int tlen = 0;
    while (target[tlen] && tlen < USER_STR_MAX - 1) { tgt_buf[tlen] = target[tlen]; tlen++; }
    tgt_buf[tlen] = '\0';
    if (tlen == 0) return -1;

    vfs_node_t* src_node = _resolve_path(src_buf);
    if (!src_node) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(tgt_buf, basename, 128);
    if (!parent || !basename[0]) return -1;

    return vfs_mount(parent, basename, src_node);
}

// umount() — unmount a filesystem from a directory (root only)
int sys_umount(char* target) {
    if (!validate_user_str(target)) return -1;
    if (!current_task) return -1;
    if (current_task->proc->euid != 0) return -1;

    char tgt_buf[USER_STR_MAX];
    int tlen = 0;
    while (target[tlen] && tlen < USER_STR_MAX - 1) { tgt_buf[tlen] = target[tlen]; tlen++; }
    tgt_buf[tlen] = '\0';
    if (tlen == 0) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(tgt_buf, basename, 128);
    if (!parent || !basename[0]) return -1;

    return vfs_umount(parent, basename);
}

// reboot() — restart or power off the system (root only)
int sys_reboot(uint32_t cmd) {
    if (!current_task) return -1;
    if (current_task->proc->euid != 0) return -1;   // root only

    __asm__ volatile ("cli");   // disable interrupts

    // Attempt ACPI power-off via PCI/PM ports
    if (cmd == REBOOT_POWEROFF) {
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    }

    // Keyboard controller reset (pulse CPU reset line)
    uint8_t tmp;
    do {
        __asm__ volatile ("inb %1, %0" : "=a"(tmp) : "Nd"((uint16_t)0x64));
    } while (tmp & 0x02);   // wait for input buffer empty
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    // If reset fails, halt forever
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

// uname() — get system identification information
int sys_uname(struct utsname_k* buf) {
    if (!validate_user_ptr(buf, sizeof(struct utsname_k))) return -1;
    _kstrcpy(buf->sysname,  "CactOS",  65);
    _kstrcpy(buf->nodename, "cact",    65);
    _kstrcpy(buf->release,  "0.1.0",   65);   // update with kernel_version if desired
    _kstrcpy(buf->version,  "#1",      65);
    _kstrcpy(buf->machine,  "i686",    65);
    return 0;
}