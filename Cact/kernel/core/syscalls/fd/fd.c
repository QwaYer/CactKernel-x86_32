#include "fd.h"
#include "validate.h"
#include "helper.h"
#include "pipe.h"
#include "path/path.h"
#include "file/file.h"
#include "kernel.h"   // terminal_winsize

#define SETFL_MASK  (0x0800 | 0x0400)
#define OPEN_ACCMODE 0x0003
#define OPEN_WRONLY  0x0001
#define OPEN_RDWR    0x0002
#define OPEN_CREAT   0x0040
#define OPEN_TRUNC   0x0200

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

int sys_ioctl(struct syscall_frame *regs) {
    int      fd  = (int)regs->ebx;
    uint32_t cmd = regs->ecx;
    void    *arg = (void *)regs->edx;

    file_t *f = _get_file(fd);

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
