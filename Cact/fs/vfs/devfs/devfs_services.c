#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "task.h"
#include "pipe.h"
#include "socket.h"
#include "unix_sock.h"
#include "rust_net_ffi.h"
#include "validate.h"
#include "helper.h"
#include "ioctl_abi.h"
#include "module/kmod.h"
#include "klog.h"
#include "memfd.h"

// devfs_services.c — kernel-service devices exposed through devfs.
//
// These turn "everything else" (console output, mount/reboot, module loading,
// socket creation, pipe creation) into plain VFS nodes so the kernel only
// needs the minimal core syscall set.  All handlers run in the calling
// process context (ioctl/write/read on the device), so current_task and its
// fd table are available.
//
//   /dev/console  write  -> kernel console (replaces sys_print / tty print)
//   /dev/sys      ioctl  -> mount/umount/reboot/module load  (root only)
//   /dev/net      ioctl  -> socket()/ping/dns/netcfg
//   /dev/pipe     ioctl  -> pipe() creation
//   /dev/memfd    ioctl  -> memfd_create()

#ifndef EPERM
#define EPERM 1
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EMFILE
#define EMFILE 24
#endif

static int _root_only(void) {
    if (!current_task || !current_task->proc) return -1;
    return current_task->proc->euid != 0 ? -EPERM : 0;
}

// ── /dev/console ──────────────────────────────────────────────────────────

static int _console_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;(void)off;(void)size;(void)buf;
    return 0;   // console has no input
}

static int _console_write(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;(void)off;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    char tmp[256];
    uint32_t i = 0;
    while (i < size) {
        uint32_t c = size - i;
        if (c >= sizeof(tmp)) c = sizeof(tmp) - 1;
        memcpy(tmp, buf + i, c);
        tmp[c] = '\0';
        printk(tmp);
        i += c;
    }
    return (int)size;
}

devfs_driver_t drv_console = { .read = _console_read, .write = _console_write };

// ── /dev/kmsg ─────────────────────────────────────────────────────────────
// Kernel message log: the plain-text boot log captured by klog (all printk
// console output).  Offset-based reads: cat /dev/kmsg dumps the whole log.

static int _kmsg_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    return klog_read(off, size, buf);
}

static int _kmsg_status(void *p, char *buf, uint32_t size) {
    (void)p;
    char nb[16];
    uint32_t n = 0;
    const char *s = "device: kmsg\ntype: kernel message log\n";
    while (s[n] && n < size - 1) { buf[n] = s[n]; n++; }
    s = "lines: ";
    for (int i = 0; s[i] && n < size - 1; i++) buf[n++] = s[i];
    snprintf(nb, sizeof(nb), "%d", (int)klog_line_count());
    for (int i = 0; nb[i] && n < size - 1; i++) buf[n++] = nb[i];
    s = "\nbytes: ";
    for (int i = 0; s[i] && n < size - 1; i++) buf[n++] = s[i];
    snprintf(nb, sizeof(nb), "%d", (int)klog_available());
    for (int i = 0; nb[i] && n < size - 1; i++) buf[n++] = nb[i];
    s = "\ndropped: ";
    for (int i = 0; s[i] && n < size - 1; i++) buf[n++] = s[i];
    snprintf(nb, sizeof(nb), "%d", (int)klog_dropped_bytes());
    for (int i = 0; nb[i] && n < size - 1; i++) buf[n++] = nb[i];
    if (n < size) buf[n++] = '\n';
    if (n < size) buf[n] = '\0';
    return (int)n;
}

devfs_driver_t drv_kmsg = { .read = _kmsg_read, .status = _kmsg_status };

// ── /dev/sys ──────────────────────────────────────────────────────────────

static int _sys_mount_ioctl(cact_mount_arg_t *a) {
    int r = _root_only();
    if (r) return r;

    char *ksrc = copy_path_from_user(a->src);
    if (!ksrc) return -EFAULT;
    char *ktgt = copy_path_from_user(a->target);
    if (!ktgt) { kfree(ksrc); return -EFAULT; }

    vfs_node_t *src_node = vfs_resolve_path(ksrc);
    kfree(ksrc);
    if (!src_node) { kfree(ktgt); return -1; }

    char basename[128];
    vfs_node_t *parent = vfs_resolve_parent(ktgt, basename, 128);
    kfree(ktgt);
    if (!parent || !basename[0]) return -1;

    return vfs_mount(parent, basename, src_node);
}

static int _sys_umount_ioctl(char *target) {
    int r = _root_only();
    if (r) return r;

    char *ktgt = copy_path_from_user(target);
    if (!ktgt) return -EFAULT;

    char basename[128];
    vfs_node_t *parent = vfs_resolve_parent(ktgt, basename, 128);
    kfree(ktgt);
    if (!parent || !basename[0]) return -1;

    return vfs_umount(parent, basename);
}

static void _do_reboot(uint32_t cmd) {
    __asm__ volatile ("cli");

    if (cmd == CACT_REBOOT_POWEROFF) {
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    }

    uint8_t tmp;
    do {
        __asm__ volatile ("inb %1, %0" : "=a"(tmp) : "Nd"((uint16_t)0x64));
    } while (tmp & 0x02);
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    for (;;) __asm__ volatile ("hlt");
}

static int _sys_ioctl(void *p, uint32_t cmd, void *arg) {
    (void)p;

    switch (cmd) {
    case CACT_SYSCTL_MOUNT: {
        cact_mount_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        return _sys_mount_ioctl(&a);
    }

    case CACT_SYSCTL_UMOUNT: {
        if (!validate_user_str((const char *)arg)) return -EFAULT;
        return _sys_umount_ioctl((char *)arg);
    }

    case CACT_SYSCTL_REBOOT: {
        uint32_t c;
        if (!arg) return -EINVAL;
        if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;
        int r = _root_only();
        if (r) return r;
        _do_reboot(c);
        return 0;   // never reached
    }

    case CACT_SYSCTL_MODULE_LOAD: {
        cact_module_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        int r = _root_only();
        if (r) return r;
        char *kpath = copy_path_from_user(a.path);
        if (!kpath) return -EFAULT;
        int rc = kmod_load_kpath(kpath, a.vendor_id, a.device_id);
        kfree(kpath);
        return rc;
    }

    case CACT_SYSCTL_MODULE_UNLOAD: {
        int r = _root_only();
        if (r) return r;
        if (!arg) return kmod_unload_kname(0);
        if (!validate_user_str((const char *)arg)) return -EFAULT;
        char *kname = copy_path_from_user((const char *)arg);
        if (!kname) return -EFAULT;
        int rc = kmod_unload_kname(kname);
        kfree(kname);
        return rc;
    }

    default:
        return -EINVAL;
    }
}

devfs_driver_t drv_sys = { .ioctl = _sys_ioctl };

// ── /dev/net ──────────────────────────────────────────────────────────────

static int _net_ioctl(void *p, uint32_t cmd, void *arg) {
    (void)p;

    switch (cmd) {
    case CACT_NETCTL_SOCKET: {
        cact_socket_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!current_task) return -1;
        vfs_node_t *node;
        if ((int)a.domain == AF_UNIX)
            node = unix_sock_create((int)a.type, (int)a.proto);
        else
            node = ksock_create((int)a.domain, (int)a.type, (int)a.proto);
        if (!node) return -1;
        return alloc_fd(node);
    }

    case CACT_NETCTL_SOCKETPAIR: {
        cact_socketpair_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        int fds[2];
        int r = unix_socketpair((int)a.type, fds);
        if (r < 0) return r;
        a.fds[0] = (uint32_t)fds[0];
        a.fds[1] = (uint32_t)fds[1];
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_NETCTL_PING: {
        cact_ping_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        return rust_net_ping_echo_host(a.dst_ip, (uint16_t)a.id, (uint16_t)a.seq);
    }

    case CACT_NETCTL_DNS_RESOLVE: {
        cact_dns_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!validate_user_str(a.name)) return -EFAULT;
        if (!validate_user_ptr(a.out_ip, sizeof(uint32_t))) return -EFAULT;
        return rust_net_dns_resolve_a(a.name, a.out_ip);
    }

    case CACT_NETCTL_NETCFG: {
        int r = _root_only();
        if (r) return r;
        cact_netcfg_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        return rust_net_set_ipv4_config(a.ip_host, a.netmask_host,
                                        a.gateway_host, a.dns_host);
    }

    case CACT_NETCTL_NETCFG_GET: {
        if (!arg) return -EINVAL;
        if (!validate_user_ptr(arg, sizeof(cact_netcfg_get_t))) return -EFAULT;
        cact_netcfg_get_t g;
        memset(&g, 0, sizeof(g));
        rust_net_get_ipv4_config(&g.ip_host, &g.netmask_host,
                                 &g.gateway_host, &g.dns_host);
        g.link_up = (rust_net_link_is_up() > 0) ? 1u : 0u;
        if (g.link_up)
            rust_net_get_mac(g.mac);
        return copy_to_user(arg, &g, sizeof(g));
    }

    default:
        return -EINVAL;
    }
}

devfs_driver_t drv_net = { .ioctl = _net_ioctl };

// ── /dev/pipe ─────────────────────────────────────────────────────────────

static int _pipe_ioctl(void *p, uint32_t cmd, void *arg) {
    (void)p;
    if (cmd != CACT_PIPECTL_CREATE) return -EINVAL;
    if (!arg) return -EINVAL;
    if (!validate_user_ptr(arg, sizeof(uint32_t) * 2)) return -EFAULT;
    if (!current_task) return -1;

    vfs_node_t *pipefd[2];
    if (pipe_create(pipefd, 0) != 0) return -1;

    file_t *rf = file_alloc(pipefd[0]);
    file_t *wf = file_alloc(pipefd[1]);
    if (!rf || !wf) {
        if (rf) file_unref(rf);
        if (wf) file_unref(wf);
        close_vfs(pipefd[0]);
        close_vfs(pipefd[1]);
        return -ENOMEM;
    }

    int rfd = -1, wfd = -1;
    for (int i = 3; i < MAX_FD && (rfd < 0 || wfd < 0); i++) {
        if (!current_task->proc->fds->files[i]) {
            if (rfd < 0) rfd = i;
            else         wfd = i;
        }
    }

    if (rfd < 0 || wfd < 0) {
        file_unref(rf);
        file_unref(wf);
        return -EMFILE;
    }

    current_task->proc->fds->files[rfd] = rf;
    current_task->proc->fds->files[wfd] = wf;

    uint32_t fds[2] = { (uint32_t)rfd, (uint32_t)wfd };
    return copy_to_user(arg, fds, sizeof(fds));
}

devfs_driver_t drv_pipe = { .ioctl = _pipe_ioctl };

// ── /dev/memfd ────────────────────────────────────────────────────────────

static int _memfd_ioctl(void *p, uint32_t cmd, void *arg) {
    (void)p;
    if (cmd != CACT_MEMFDCTL_CREATE) return -EINVAL;
    if (!arg) return -EINVAL;
    cact_memfd_create_arg_t a;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (!current_task) return -1;

    char name[128];
    name[0] = 0;
    if (a.name) {
        if (!validate_user_str(a.name)) return -EFAULT;
        if (copy_from_user(name, a.name, sizeof(name) - 1) != 0) return -EFAULT;
        name[sizeof(name) - 1] = 0;
    }

    vfs_node_t *node = memfd_create_vnode(name[0] ? name : "memfd", (int)a.flags);
    if (!node) return -ENOMEM;

    int fd = alloc_fd(node);
    if (fd < 0) return -EMFILE;   // alloc_fd released the node again

    current_task->proc->fds->files[fd]->cloexec =
        (a.flags & CACT_MFD_CLOEXEC) ? 1 : 0;
    return fd;
}

devfs_driver_t drv_memfd = { .ioctl = _memfd_ioctl };
