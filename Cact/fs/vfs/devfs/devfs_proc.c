#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"
#include "task.h"
#include "validate.h"
#include "tty.h"
#include "pty.h"
#include "ioctl_abi.h"

// devfs_proc.c — per-process nodes and the pty namespace.
//
// These nodes cannot be expressed as a devfs_driver_t: what they refer to is
// decided per open() (the calling process's fd table, its controlling
// terminal), so they publish ready-made VFS nodes with their own fops and let
// devfs_add_node() link them into /dev.
//
//   /dev/tty      controlling terminal of the caller (active VT if none)
//   /dev/stdin    per-process alias of fd 0   (dup() semantics: shares offset)
//   /dev/stdout   per-process alias of fd 1
//   /dev/stderr   per-process alias of fd 2
//   /dev/fd/N     per-process alias of fd N
//   /dev/core     the calling process's own address space
//   /dev/ptmx     allocate a pty master
//   /dev/pts/N    the slave end of pty N

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

/* Linux pty ioctls, accepted alongside the Cact ones so the usual userspace
 * numbers keep working. */
#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

// ── /dev/tty ──────────────────────────────────────────────────────────────
// Resolved once per open(): the VT the process calls its terminal.  A process
// that has none yet adopts the active VT, so the boot path (cgoct opens
// /dev/tty before any session setup exists) keeps working.

static void _tty_alias_open(vfs_node_t *node, file_t *f) {
    (void)node;
    if (!current_task) return;
    if (tty_get_ctty() == 0)
        tty_set_ctty(0);
    f->priv = (void *)(uintptr_t)tty_get_ctty();
}

static int _tty_alias_read(vfs_node_t *node, void *priv, uint32_t off,
                           uint32_t size, char *buf) {
    (void)node;
    return tty_read((int)(uintptr_t)priv, off, size, buf);
}

static int _tty_alias_write(vfs_node_t *node, void *priv, uint32_t off,
                            uint32_t size, char *buf) {
    (void)node;
    return tty_write((int)(uintptr_t)priv, off, size, buf);
}

static int _tty_alias_ioctl(vfs_node_t *node, void *priv, uint32_t cmd,
                            void *arg) {
    (void)node;
    return tty_ioctl((int)(uintptr_t)priv, cmd, arg);
}

static vfs_file_ops_t tty_alias_fops = {
    .read = _tty_alias_read,
    .write = _tty_alias_write,
    .ioctl = _tty_alias_ioctl,
    .open  = _tty_alias_open,
};

static vfs_node_t tty_node;

// ── /dev/stdin, /dev/stdout, /dev/stderr, /dev/fd/N ───────────────────────
// An alias holds a reference to the target file description, so reads/writes
// share its offset exactly like dup().

static void _fd_alias_open(vfs_node_t *node, file_t *f) {
    f->priv = 0;
    if (!current_task || !current_task->proc || !current_task->proc->fds)
        return;
    int fd = (int)(uintptr_t)node->priv;
    if (fd < 0 || fd >= MAX_FD) return;
    file_t *target = current_task->proc->fds->files[fd];
    if (target) f->priv = file_ref(target);
}

static int _fd_alias_read(vfs_node_t *node, void *priv, uint32_t off,
                          uint32_t size, char *buf) {
    (void)node; (void)off;
    if (!priv) return -1;
    file_t *t = (file_t *)priv;
    int r = read_file_vfs(t, t->offset, size, buf);
    if (r > 0) t->offset += (uint32_t)r;
    return r;
}

static int _fd_alias_write(vfs_node_t *node, void *priv, uint32_t off,
                           uint32_t size, char *buf) {
    (void)node; (void)off;
    if (!priv) return -1;
    file_t *t = (file_t *)priv;
    int r = write_file_vfs(t, t->offset, size, buf);
    if (r > 0) t->offset += (uint32_t)r;
    return r;
}

static int _fd_alias_ioctl(vfs_node_t *node, void *priv, uint32_t cmd,
                           void *arg) {
    (void)node;
    if (!priv) return -1;
    return ioctl_file_vfs((file_t *)priv, cmd, arg);
}

static int _fd_alias_poll(vfs_node_t *node, void *priv, uint32_t events) {
    (void)node;
    if (!priv) return 0;
    return poll_file_vfs((file_t *)priv, events);
}

static void _fd_alias_release(vfs_node_t *node, file_t *f) {
    (void)node;
    if (f->priv) {
        file_unref((file_t *)f->priv);
        f->priv = 0;
    }
}

static vfs_file_ops_t fd_alias_fops = {
    .read    = _fd_alias_read,
    .write   = _fd_alias_write,
    .ioctl   = _fd_alias_ioctl,
    .poll    = _fd_alias_poll,
    .open    = _fd_alias_open,
    .release = _fd_alias_release,
};

static vfs_node_t stdin_node, stdout_node, stderr_node;

// /dev/fd is a directory of the process's descriptors.
static vfs_node_t   fd_dir;
static vfs_node_t   fd_nodes[MAX_FD];
static vfs_dirent_t fd_de;

static int _fd_parse(const char *name) {
    if (!name || !name[0]) return -1;
    int n = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
        n = n * 10 + (name[i] - '0');
        if (n >= MAX_FD) return -1;
    }
    return n;
}

static vfs_node_t *_fd_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    int fd = _fd_parse(name);
    if (fd < 0) return 0;
    return &fd_nodes[fd];
}

static vfs_dirent_t *_fd_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    if (!current_task || !current_task->proc || !current_task->proc->fds)
        return 0;
    uint32_t seen = 0;
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!current_task->proc->fds->files[fd]) continue;
        if (seen++ == index) {
            snprintf(fd_de.name, sizeof(fd_de.name), "%d", fd);
            fd_de.inode = (uint32_t)fd;
            return &fd_de;
        }
    }
    return 0;
}

static vfs_ops_t fd_dir_ops = {
    .walk    = _fd_walk,
    .readdir = _fd_readdir,
};

// ── /dev/core ─────────────────────────────────────────────────────────────
// The calling process's address space, addressed by virtual address (the file
// offset *is* the address), i.e. what Linux 2.4's /dev/core was.

static int _core_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    if (!validate_user_ptr((void *)(uintptr_t)off, size)) return -1;
    memcpy(buf, (void *)(uintptr_t)off, size);
    return (int)size;
}

static int _core_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    if (!validate_user_ptr((void *)(uintptr_t)off, size)) return -1;
    memcpy((void *)(uintptr_t)off, buf, size);
    return (int)size;
}

static vfs_ops_t core_ops = {
    .read  = _core_read,
    .write = _core_write,
};

static vfs_node_t core_node;

// ── /dev/pts/N ────────────────────────────────────────────────────────────

static void _pts_open(vfs_node_t *node, file_t *f) {
    f->priv = 0;
    int i = (int)(uintptr_t)node->priv;
    if (!pty_used(i)) return;
    if (pty_unlocked(i) == 0) return;   // master has not released the slave
    pty_slave_opened(i);
    f->priv = (void *)(uintptr_t)(i + 1);
}

static int _pts_read(vfs_node_t *node, void *priv, uint32_t off, uint32_t size,
                     char *buf) {
    (void)node; (void)off;
    if (!priv) return -1;
    return pty_slave_read((int)(uintptr_t)priv - 1, size, buf);
}

static int _pts_write(vfs_node_t *node, void *priv, uint32_t off, uint32_t size,
                      char *buf) {
    (void)node; (void)off;
    if (!priv) return -1;
    return pty_slave_write((int)(uintptr_t)priv - 1, size, buf);
}

static void _pts_release(vfs_node_t *node, file_t *f) {
    (void)node;
    if (f->priv) {
        pty_close_slave((int)(uintptr_t)f->priv - 1);
        f->priv = 0;
    }
}

static vfs_file_ops_t pts_fops = {
    .read    = _pts_read,
    .write   = _pts_write,
    .open    = _pts_open,
    .release = _pts_release,
};

static vfs_node_t   pts_dir;
static vfs_node_t   pts_nodes[PTY_MAX];
static vfs_dirent_t pts_de;

static vfs_node_t *_pts_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    int n = _fd_parse(name);
    if (n < 0 || n >= PTY_MAX) return 0;
    if (!pty_used(n)) return 0;
    return &pts_nodes[n];
}

static vfs_dirent_t *_pts_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    uint32_t seen = 0;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_used(i)) continue;
        if (seen++ == index) {
            snprintf(pts_de.name, sizeof(pts_de.name), "%d", i);
            pts_de.inode = (uint32_t)i;
            return &pts_de;
        }
    }
    return 0;
}

static vfs_ops_t pts_dir_ops = {
    .walk    = _pts_walk,
    .readdir = _pts_readdir,
};

// ── /dev/ptmx ─────────────────────────────────────────────────────────────

static void _ptmx_open(vfs_node_t *node, file_t *f) {
    (void)node;
    int i = pty_alloc();
    f->priv = (i < 0) ? 0 : (void *)(uintptr_t)(i + 1);
}

static int _ptmx_read(vfs_node_t *node, void *priv, uint32_t off, uint32_t size,
                      char *buf) {
    (void)node; (void)off;
    if (!priv) return -1;
    return pty_master_read((int)(uintptr_t)priv - 1, size, buf);
}

static int _ptmx_write(vfs_node_t *node, void *priv, uint32_t off,
                       uint32_t size, char *buf) {
    (void)node; (void)off;
    if (!priv) return -1;
    return pty_master_write((int)(uintptr_t)priv - 1, size, buf);
}

static int _ptmx_ioctl(vfs_node_t *node, void *priv, uint32_t cmd, void *arg) {
    (void)node;
    if (!priv) return -1;
    int i = (int)(uintptr_t)priv - 1;

    switch (cmd) {
    case CACT_PTYCTL_GET_NUMBER:
    case TIOCGPTN:
        if (!arg || !validate_user_ptr(arg, sizeof(int))) return -1;
        *(int *)arg = i;
        return 0;

    case CACT_PTYCTL_LOCK:
    case TIOCSPTLCK: {
        int v;
        if (!arg || copy_from_user(&v, arg, sizeof(v)) != 0) return -1;
        pty_lock(i, v);
        return 0;
    }

    default:
        return -1;
    }
}

static void _ptmx_release(vfs_node_t *node, file_t *f) {
    (void)node;
    if (f->priv) {
        pty_close_master((int)(uintptr_t)f->priv - 1);
        f->priv = 0;
    }
}

static vfs_file_ops_t ptmx_fops = {
    .read    = _ptmx_read,
    .write   = _ptmx_write,
    .ioctl   = _ptmx_ioctl,
    .open    = _ptmx_open,
    .release = _ptmx_release,
};

static vfs_node_t ptmx_node;

// ── registration ──────────────────────────────────────────────────────────

static void _init_node(vfs_node_t *n, const char *name, uint32_t type,
                       vfs_ops_t *ops, vfs_file_ops_t *fops, void *priv) {
    memset(n, 0, sizeof(vfs_node_t));
    strlcpy(n->name, name, 128);
    n->type  = type;
    n->ops   = ops;
    n->fops  = fops;
    n->priv  = priv;
}

void devfs_proc_init(void) {
    _init_node(&tty_node, "tty", VFS_CHARDEVICE, 0, &tty_alias_fops, 0);
    devfs_add_node("tty", &tty_node);

    _init_node(&stdin_node,  "stdin",  VFS_CHARDEVICE, 0, &fd_alias_fops,
               (void *)(uintptr_t)0);
    _init_node(&stdout_node, "stdout", VFS_CHARDEVICE, 0, &fd_alias_fops,
               (void *)(uintptr_t)1);
    _init_node(&stderr_node, "stderr", VFS_CHARDEVICE, 0, &fd_alias_fops,
               (void *)(uintptr_t)2);
    devfs_add_node("stdin",  &stdin_node);
    devfs_add_node("stdout", &stdout_node);
    devfs_add_node("stderr", &stderr_node);

    _init_node(&fd_dir, "fd", VFS_DIRECTORY, &fd_dir_ops, 0, 0);
    for (int fd = 0; fd < MAX_FD; fd++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "%d", fd);
        _init_node(&fd_nodes[fd], nm, VFS_CHARDEVICE, 0, &fd_alias_fops,
                   (void *)(uintptr_t)fd);
    }
    devfs_add_node("fd", &fd_dir);

    _init_node(&core_node, "core", VFS_CHARDEVICE, &core_ops, 0, 0);
    devfs_add_node("core", &core_node);

    _init_node(&pts_dir, "pts", VFS_DIRECTORY, &pts_dir_ops, 0, 0);
    for (int i = 0; i < PTY_MAX; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "%d", i);
        _init_node(&pts_nodes[i], nm, VFS_CHARDEVICE, 0, &pts_fops,
                   (void *)(uintptr_t)i);
    }
    devfs_add_node("pts", &pts_dir);

    _init_node(&ptmx_node, "ptmx", VFS_CHARDEVICE, 0, &ptmx_fops, 0);
    devfs_add_node("ptmx", &ptmx_node);
}
