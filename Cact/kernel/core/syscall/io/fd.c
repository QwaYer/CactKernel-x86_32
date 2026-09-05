#include "fd.h"
#include "validate.h"
#include "helper.h"
#include "pipe.h"
#include "ioctl_abi.h"
#include "kernel.h"   // terminal_winsize

// SOCKCTL_* node-ioctl dispatcher (defined in net/net.c)
int sock_ioctl_dispatch(vfs_node_t *node, uint32_t cmd, void *arg);

#define SETFL_MASK  (0x0800 | 0x0400)
#define OPEN_ACCMODE 0x0003
#define OPEN_WRONLY  0x0001
#define OPEN_RDWR    0x0002
#define OPEN_CREAT   0x0040
#define OPEN_TRUNC   0x0200

// errno values used by the generic node-ioctl ABI (returned as negative values)
#ifndef EPERM
#define EPERM   1
#endif
#ifndef ENOENT
#define ENOENT  2
#endif
#ifndef EIO
#define EIO     5
#endif
#ifndef EBADF
#define EBADF   9
#endif
#ifndef ENOMEM
#define ENOMEM  12
#endif
#ifndef EACCES
#define EACCES  13
#endif
#ifndef EFAULT
#define EFAULT  14
#endif
#ifndef EEXIST
#define EEXIST  17
#endif
#ifndef ENOTDIR
#define ENOTDIR 20
#endif
#ifndef EINVAL
#define EINVAL  22
#endif
#ifndef EMFILE
#define EMFILE  24
#endif

file_t *_get_file(int fd) {
    if (!current_task) return 0;
    if (fd < 0 || fd >= MAX_FD) return 0;
    return current_task->proc->fds->files[fd];
}

int sys_open(char *name, int flags) {
    if (!current_task) return -1;

    char *kname = copy_path_from_user(name);
    if (!kname) return -1;

    vfs_node_t *node = _resolve_path(kname);
    if (!node) {
        if (!(flags & OPEN_CREAT)) { kfree(kname); return -1; }

        char basename[128];
        vfs_node_t *parent = _resolve_parent(kname, basename, 128);
        if (!parent || !basename[0]) { kfree(kname); return -1; }

        if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) { kfree(kname); return -1; }

        if (create_vfs(parent, basename) != 0) { kfree(kname); return -1; }
        node = _resolve_path(kname);
        if (!node) { kfree(kname); return -1; }
    }

    uint32_t need = 0;
    uint32_t acc  = (uint32_t)flags & OPEN_ACCMODE;
    if (acc == 0 || acc == 2) need |= VFS_PERM_READ;
    if (acc == 1 || acc == 2) need |= VFS_PERM_WRITE;
    if (vfs_check_perm(node, need) < 0) { kfree(kname); return -1; }

    file_t *f = file_alloc(node);
    if (!f) { kfree(kname); return -1; }
    f->flags = (uint32_t)flags;

    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->proc->fds->files[i]) {
            current_task->proc->fds->files[i] = f;

            if ((flags & OPEN_TRUNC) &&
                (((flags & OPEN_ACCMODE) == OPEN_WRONLY) ||
                 ((flags & OPEN_ACCMODE) == OPEN_RDWR))) {
                if (truncate_vfs(node, 0) != 0) {
                    current_task->proc->fds->files[i] = 0;
                    file_unref(f);
                    kfree(kname);
                    return -1;
                }
            }
            kfree(kname);
            return i;
        }
    }

    file_unref(f);
    kfree(kname);
    return -1;
}

int sys_read(int fd, char *buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    file_t *f = _get_file(fd);
    if (!f || !f->node) return -1;

    int ret = read_vfs(f->node, f->offset, size, buf);
    if (ret > 0) f->offset += (uint32_t)ret;
    return ret;
}

int sys_write(int fd, char *buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    file_t *f = _get_file(fd);
    if (!f || !f->node) return -1;

    int ret = write_vfs(f->node, f->offset, size, buf);
    if (ret > 0) f->offset += (uint32_t)ret;
    return ret;
}

int sys_close(int fd) {
    file_t *f = _get_file(fd);
    if (!f) return -1;
    current_task->proc->fds->files[fd] = 0;
    file_unref(f);
    return 0;
}

int sys_lseek(struct syscall_frame *regs) {
    int fd     = (int)regs->ebx;
    int offset = (int)regs->ecx;
    int whence = (int)regs->edx;

    file_t *f = _get_file(fd);
    if (!f || !f->node) return -1;

    uint32_t new_off;
    if (f->node->ops && f->node->ops->lseek) {
        if (f->node->ops->lseek(f->node, offset, whence, &new_off) == 0) {
            f->offset = new_off;
            return (int)new_off;
        }
        return -1;
    }

    switch (whence) {
        case 0: // SEEK_SET
            if (offset < 0) return -1;
            new_off = (uint32_t)offset;
            break;
        case 1: // SEEK_CUR:
            new_off = (uint32_t)((int)f->offset + offset);
            break;
        case 2: // SEEK_END:
            new_off = f->node->size + (uint32_t)offset;
            break;
        default:
            return -1;
    }
    f->offset = new_off;
    return (int)new_off;
}

// =========================================================================
// Generic node-ioctl ABI (ioctl_abi.h): FDCTL_* (any fd) and DIRCTL_* (dir).
// These commands are the *new* ABI: once the legacy per-name syscalls are
// dropped, dup/lseek/fcntl/stat/mkdir/... are reached through ioctl().
// =========================================================================

static int _copy_name_arg(const char *u, char **out) {
    if (!validate_user_str(u)) return -1;
    *out = copy_path_from_user(u);
    return *out ? 0 : -1;
}

// A single path component is required for dirfd-relative namespace ops.
static int _name_is_single(const char *s) {
    if (!s || !s[0]) return 0;
    for (int i = 0; s[i]; i++)
        if (s[i] == '/') return 0;
    return 1;
}

static int _fdctl_handle(file_t *f, int fd, uint32_t cmd, void *arg) {
    switch (cmd) {
    case CACT_FDCTL_DUP:
        return sys_dup(fd);

    case CACT_FDCTL_DUP2: {
        cact_fd_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if ((int)a.newfd < 0 || a.newfd >= MAX_FD) return -EBADF;
        if (fd == (int)a.newfd) return (int)a.newfd;
        file_ref(f);
        file_t *old = current_task->proc->fds->files[a.newfd];
        current_task->proc->fds->files[a.newfd] = f;
        if (old) file_unref(old);
        return (int)a.newfd;
    }

    case CACT_FDCTL_FCNTL: {
        cact_fcntl_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        return sys_fcntl(fd, (int)a.cmd, (int)a.arg);
    }

    case CACT_FDCTL_LSEEK: {
        cact_lseek_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

        int      offset = (int)a.offset;
        uint32_t new_off;

        if (f->node->ops && f->node->ops->lseek) {
            if (f->node->ops->lseek(f->node, offset, (int)a.whence, &new_off) == 0) {
                f->offset = new_off;
                return (int)new_off;
            }
            return -1;
        }

        switch (a.whence) {
        case 0: // SEEK_SET
            if (offset < 0) return -EINVAL;
            new_off = (uint32_t)offset;
            break;
        case 1: // SEEK_CUR
            new_off = (uint32_t)((int)f->offset + offset);
            break;
        case 2: // SEEK_END
            new_off = f->node->size + (uint32_t)offset;
            break;
        default:
            return -EINVAL;
        }
        f->offset = new_off;
        return (int)new_off;
    }

    case CACT_FDCTL_FSTAT:
        if (!arg || !validate_user_ptr(arg, 16)) return -EFAULT;
        return stat_vfs(f->node, (uint32_t *)arg);

    case CACT_FDCTL_FTRUNCATE: {
        uint32_t len;
        if (!arg) return -EINVAL;
        if (copy_from_user(&len, arg, sizeof(len)) != 0) return -EFAULT;
        if (f->node->type != VFS_FILE) return -EINVAL;
        return truncate_vfs(f->node, len);
    }

    case CACT_FDCTL_GETDENTS: {
        cact_getdents_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!a.buf || a.count == 0) return -EINVAL;
        if (f->node->type != VFS_DIRECTORY) return -ENOTDIR;

        uint32_t entry_size = sizeof(struct cact_dirent);
        if (a.count < entry_size) return -EINVAL;
        if (!validate_user_ptr(a.buf, a.count)) return -EFAULT;

        uint32_t written = 0;
        uint32_t index   = f->offset;

        while (written + entry_size <= a.count) {
            struct vfs_dirent *de = readdir_vfs(f->node, index);
            if (!de) break;

            struct cact_dirent local;
            local.d_ino = de->inode;
            int i = 0;
            while (de->name[i] && i < 123) { local.d_name[i] = de->name[i]; i++; }
            local.d_name[i] = '\0';

            if (copy_to_user((char *)a.buf + written, &local, entry_size) != 0) break;
            written += entry_size;
            index++;
        }

        f->offset = index;
        return (int)written;
    }

    case CACT_FDCTL_FSYNC:
        return 0;

    default:
        return -EINVAL;
    }
}

static int _dir_wx(vfs_node_t *dir) {
    return vfs_check_perm(dir, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0 ? -EACCES : 0;
}

static int _dirctl_openat(vfs_node_t *dir, char *kname, uint32_t flags) {
    vfs_node_t *node = finddir_vfs(dir, kname);
    if (!node) {
        if (!(flags & OPEN_CREAT)) return -ENOENT;
        int p = _dir_wx(dir);
        if (p) return p;
        if (create_vfs(dir, kname) != 0) return -EIO;
        node = finddir_vfs(dir, kname);
        if (!node) return -EIO;
    }

    uint32_t acc  = flags & OPEN_ACCMODE;
    uint32_t need = 0;
    if (acc == 0 || acc == 2) need |= VFS_PERM_READ;
    if (acc == 1 || acc == 2) need |= VFS_PERM_WRITE;
    if (vfs_check_perm(node, need) < 0) return -EACCES;

    file_t *file = file_alloc(node);
    if (!file) return -ENOMEM;
    file->flags = flags;

    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->proc->fds->files[i]) {
            current_task->proc->fds->files[i] = file;
            if ((flags & OPEN_TRUNC) &&
                (acc == OPEN_WRONLY || acc == OPEN_RDWR)) {
                if (truncate_vfs(node, 0) != 0) {
                    current_task->proc->fds->files[i] = 0;
                    file_unref(file);
                    return -EIO;
                }
            }
            return i;
        }
    }

    file_unref(file);
    return -EMFILE;
}

static int _dirctl_handle(vfs_node_t *dir, uint32_t cmd, void *arg) {
    switch (cmd) {
    case CACT_DIRCTL_OPENAT: {
        cact_openat_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        int ret = _dirctl_openat(dir, kname, a.flags);
        kfree(kname);
        return ret;
    }

    case CACT_DIRCTL_CREATE: {
        cact_openat_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        if (finddir_vfs(dir, kname)) { kfree(kname); return -EEXIST; }
        int p = _dir_wx(dir);
        int ret = p ? p : create_vfs(dir, kname);
        kfree(kname);
        return ret;
    }

    case CACT_DIRCTL_MKDIR: {
        char *kname;
        if (_copy_name_arg((const char *)arg, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        if (finddir_vfs(dir, kname)) { kfree(kname); return -EEXIST; }
        int p = _dir_wx(dir);
        int ret = p ? p : mkdir_vfs(dir, kname);
        kfree(kname);
        return ret;
    }

    case CACT_DIRCTL_RMDIR: {
        char *kname;
        if (_copy_name_arg((const char *)arg, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        if (!finddir_vfs(dir, kname)) { kfree(kname); return -ENOENT; }
        int p = _dir_wx(dir);
        int ret = p ? p : rmdir_vfs(dir, kname);
        kfree(kname);
        return ret;
    }

    case CACT_DIRCTL_UNLINK: {
        char *kname;
        if (_copy_name_arg((const char *)arg, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        if (!finddir_vfs(dir, kname)) { kfree(kname); return -ENOENT; }
        int p = _dir_wx(dir);
        int ret = p ? p : vfs_unlink(dir, kname);
        kfree(kname);
        return ret;
    }

    case CACT_DIRCTL_RENAME: {
        cact_rename_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kold, *knew;
        if (_copy_name_arg(a.oldname, &kold) != 0) return -EFAULT;
        if (_copy_name_arg(a.newname, &knew) != 0) { kfree(kold); return -EFAULT; }
        if (!_name_is_single(kold) || !_name_is_single(knew)) {
            kfree(kold); kfree(knew); return -EINVAL;
        }
        if (!finddir_vfs(dir, kold)) { kfree(kold); kfree(knew); return -ENOENT; }
        int p = _dir_wx(dir);
        int ret = p ? p : rename_vfs(dir, kold, knew);
        kfree(kold);
        kfree(knew);
        return ret;
    }

    case CACT_DIRCTL_LINK: {
        cact_link_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *ktarget, *knew;
        if (_copy_name_arg(a.target, &ktarget) != 0) return -EFAULT;
        if (_copy_name_arg(a.newname, &knew) != 0) { kfree(ktarget); return -EFAULT; }
        if (!_name_is_single(ktarget) || !_name_is_single(knew)) {
            kfree(ktarget); kfree(knew); return -EINVAL;
        }
        vfs_node_t *target_node = finddir_vfs(dir, ktarget);
        if (!target_node) { kfree(ktarget); kfree(knew); return -ENOENT; }
        if (finddir_vfs(dir, knew)) { kfree(ktarget); kfree(knew); return -EEXIST; }
        int p = _dir_wx(dir);
        int ret = p ? p : vfs_link(dir, knew, target_node);
        kfree(ktarget);
        kfree(knew);
        return ret;
    }

    case CACT_DIRCTL_SYMLINK: {
        cact_symlink_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *ktarget, *kname;
        if (_copy_name_arg(a.target, &ktarget) != 0) return -EFAULT;
        if (_copy_name_arg(a.linkname, &kname) != 0) { kfree(ktarget); return -EFAULT; }
        if (!_name_is_single(kname)) { kfree(ktarget); kfree(kname); return -EINVAL; }
        if (finddir_vfs(dir, kname)) { kfree(ktarget); kfree(kname); return -EEXIST; }
        int p = _dir_wx(dir);
        int ret = p ? p : vfs_symlink(dir, kname, ktarget);
        kfree(ktarget);
        kfree(kname);
        return ret;
    }

    case CACT_DIRCTL_READLINK: {
        cact_readlink_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!a.buf || a.len == 0) return -EINVAL;
        if (!validate_user_ptr(a.buf, a.len)) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        vfs_node_t *node = finddir_vfs(dir, kname);
        kfree(kname);
        if (!node) return -ENOENT;
        if (node->type != VFS_SYMLINK) return -EINVAL;
        return vfs_readlink_node(node, (char *)a.buf, a.len);
    }

    case CACT_DIRCTL_STAT: {
        cact_statat_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!a.buf || !validate_user_ptr(a.buf, 16)) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        vfs_node_t *node = finddir_vfs(dir, kname);
        kfree(kname);
        if (!node) return -ENOENT;
        return stat_vfs(node, (uint32_t *)a.buf);
    }

    case CACT_DIRCTL_ACCESS: {
        cact_access_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        vfs_node_t *node = finddir_vfs(dir, kname);
        kfree(kname);
        if (!node) return -ENOENT;
        if (a.mode == 0) return 0;
        uint32_t perm = 0;
        if (a.mode & 4) perm |= VFS_PERM_READ;
        if (a.mode & 2) perm |= VFS_PERM_WRITE;
        if (a.mode & 1) perm |= VFS_PERM_EXEC;
        return vfs_check_perm(node, perm) < 0 ? -EACCES : 0;
    }

    case CACT_DIRCTL_CHMOD: {
        cact_chmod_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        vfs_node_t *node = finddir_vfs(dir, kname);
        kfree(kname);
        if (!node) return -ENOENT;
        if (current_task->proc->euid != 0 && current_task->proc->euid != node->uid)
            return -EPERM;
        return chmod_vfs(node, a.mode);
    }

    case CACT_DIRCTL_CHOWN: {
        cact_chown_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (current_task->proc->euid != 0) return -EPERM;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        vfs_node_t *node = finddir_vfs(dir, kname);
        kfree(kname);
        if (!node) return -ENOENT;
        return chown_vfs(node, a.uid, a.gid);
    }

    case CACT_DIRCTL_TRUNCATE: {
        cact_truncate_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        vfs_node_t *node = finddir_vfs(dir, kname);
        kfree(kname);
        if (!node) return -ENOENT;
        if (node->type != VFS_FILE) return -EINVAL;
        if (vfs_check_perm(node, VFS_PERM_WRITE) < 0) return -EACCES;
        return truncate_vfs(node, a.length);
    }

    case CACT_DIRCTL_MKNOD: {
        cact_mknod_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        char *kname;
        if (_copy_name_arg(a.name, &kname) != 0) return -EFAULT;
        if (!_name_is_single(kname)) { kfree(kname); return -EINVAL; }
        if (finddir_vfs(dir, kname)) { kfree(kname); return -EEXIST; }
        int ret = mknod_vfs(dir, kname, a.mode, a.dev);
        kfree(kname);
        return ret;
    }

    default:
        return -EINVAL;
    }
}

int sys_ioctl(struct syscall_frame *regs) {
    int      fd  = (int)regs->ebx;
    uint32_t cmd = regs->ecx;
    void    *arg = (void *)regs->edx;

    file_t *f = _get_file(fd);

    // Generic node-ioctl ABI: fd-level and directory-level commands.
    if (cmd >= 0x3000 && cmd <= 0x30FF) {
        if (!f || !f->node) return -EBADF;
        return _fdctl_handle(f, fd, cmd, arg);
    }
    if (cmd >= 0x3100 && cmd <= 0x31FF) {
        if (!f || !f->node) return -EBADF;
        if (f->node->type != VFS_DIRECTORY) return -ENOTDIR;
        return _dirctl_handle(f->node, cmd, arg);
    }
    if (cmd >= 0x3300 && cmd <= 0x33FF) {
        if (!f || !f->node) return -EBADF;
        if (f->node->type != VFS_SOCKET) return -EINVAL;
        return sock_ioctl_dispatch(f->node, cmd, arg);
    }

    if (cmd == TIOCGWINSZ || cmd == TIOCSWINSZ) {
        if (!arg || !validate_user_ptr(arg, sizeof(struct winsize))) return -1;
        struct winsize ws_buf;
        if (cmd == TIOCSWINSZ) {
            if (copy_from_user(&ws_buf, arg, sizeof(ws_buf)) != 0) return -1;
            terminal_winsize.ws_row    = ws_buf.ws_row;
            terminal_winsize.ws_col    = ws_buf.ws_col;
            terminal_winsize.ws_xpixel = ws_buf.ws_xpixel;
            terminal_winsize.ws_ypixel = ws_buf.ws_ypixel;
            if (terminal_fg_pid)
                task_signal(terminal_fg_pid, SIGWINCH);
        } else {
            ws_buf.ws_row    = terminal_winsize.ws_row;
            ws_buf.ws_col    = terminal_winsize.ws_col;
            ws_buf.ws_xpixel = terminal_winsize.ws_xpixel;
            ws_buf.ws_ypixel = terminal_winsize.ws_ypixel;
            if (copy_to_user(arg, &ws_buf, sizeof(ws_buf)) != 0) return -1;
        }
        return 0;
    }

    if (!f || !f->node) return -1;

    if (arg && !validate_user_ptr(arg, 1)) return -1;
    return ioctl_vfs(f->node, cmd, arg);
}

int sys_fcntl(int fd, int cmd, int arg) {
    file_t *f = _get_file(fd);
    if (!f) return -1;

    switch (cmd) {
    case 0: {  // F_DUPFD
        if (arg < 0 || arg >= MAX_FD) return -1;
        for (int i = arg; i < MAX_FD; i++) {
            if (!current_task->proc->fds->files[i]) {
                current_task->proc->fds->files[i] = file_ref(f);
                return i;
            }
        }
        return -1;
    }
    case 1:  // F_GETFD
        return (int)f->cloexec;
    case 2:  // F_SETFD
        f->cloexec = (arg & 1) ? 1 : 0;
        return 0;
    case 3:  // F_GETFL
        return (int)f->flags;
    case 4: { // F_SETFL
        uint32_t new_flags = (f->flags & ~(uint32_t)SETFL_MASK)
                           | ((uint32_t)arg & SETFL_MASK);
        f->flags = new_flags;
        return 0;
    }
    default:
        return -1;
    }
}

int sys_dup(int oldfd) {
    file_t *f = _get_file(oldfd);
    if (!f) return -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (!current_task->proc->fds->files[i]) {
            current_task->proc->fds->files[i] = file_ref(f);
            return i;
        }
    }
    return -1;
}

int sys_dup2(struct syscall_frame *regs) {
    int oldfd = (int)regs->ebx;
    int newfd = (int)regs->ecx;

    file_t *f = _get_file(oldfd);
    if (!f) return -1;
    if (oldfd == newfd) return newfd;

    file_ref(f);

    file_t *old = current_task->proc->fds->files[newfd];
    current_task->proc->fds->files[newfd] = f;

    if (old) file_unref(old);
    return newfd;
}
