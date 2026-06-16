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

static inline file_t *_get_file(int fd) {
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
        if (!(flags & OPEN_CREAT)) { kfree_heap(kname); return -1; }

        char basename[128];
        vfs_node_t *parent = _resolve_parent(kname, basename, 128);
        if (!parent || !basename[0]) { kfree_heap(kname); return -1; }

        if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) { kfree_heap(kname); return -1; }

        if (create_vfs(parent, basename) != 0) { kfree_heap(kname); return -1; }
        node = _resolve_path(kname);
        if (!node) { kfree_heap(kname); return -1; }
    }

    uint32_t need = 0;
    uint32_t acc  = (uint32_t)flags & OPEN_ACCMODE;
    if (acc == 0 || acc == 2) need |= VFS_PERM_READ;
    if (acc == 1 || acc == 2) need |= VFS_PERM_WRITE;
    if (vfs_check_perm(node, need) < 0) { kfree_heap(kname); return -1; }

    file_t *f = file_alloc(node);
    if (!f) { kfree_heap(kname); return -1; }
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
                    kfree_heap(kname);
                    return -1;
                }
            }
            kfree_heap(kname);
            return i;
        }
    }

    file_unref(f);
    kfree_heap(kname);
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

int sys_pipe(struct syscall_frame *regs) {
    int *user_fds = (int *)regs->ebx;
    if (!validate_user_ptr(user_fds, sizeof(int) * 2)) return -1;
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
        return -1;
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
        return -1;
    }

    current_task->proc->fds->files[rfd] = rf;
    current_task->proc->fds->files[wfd] = wf;
    user_fds[0] = rfd;
    user_fds[1] = wfd;
    return 0;
}

int sys_select(struct syscall_frame *regs) {
    sel_args_t *ua = (sel_args_t *)regs->ebx;
    if (!validate_user_ptr(ua, sizeof(sel_args_t))) return -1;
    if (!current_task) return -1;

    int              nfds  = ua->nfds;
    sel_fdset_t     *urfds = ua->readfds;
    sel_fdset_t     *uwfds = ua->writefds;
    sel_fdset_t     *uefds = ua->exceptfds;
    struct timeval_fd *utv = ua->timeout;

    if (nfds < 0 || nfds > MAX_FD) return -1;
    if (urfds && !validate_user_ptr(urfds, sizeof(sel_fdset_t))) return -1;
    if (uwfds && !validate_user_ptr(uwfds, sizeof(sel_fdset_t))) return -1;
    if (uefds && !validate_user_ptr(uefds, sizeof(sel_fdset_t))) return -1;
    if (utv && !validate_user_ptr(utv, sizeof(struct timeval_fd))) return -1;

#define TIMER_HZ_FD 100
    int      infinite    = (utv == 0);
    int      nonblocking = 0;
    uint32_t deadline    = 0;
    if (!infinite) {
        uint32_t ticks = (uint32_t)(utv->tv_sec * TIMER_HZ_FD) +
                         (uint32_t)((utv->tv_usec + (1000000 / TIMER_HZ_FD) - 1) /
                                    (1000000 / TIMER_HZ_FD));
        nonblocking = (ticks == 0);
        deadline    = timer_ticks_get() + ticks;
    }
#undef TIMER_HZ_FD

    sel_fdset_t orig_r, orig_w, orig_e;
    SEL_ZERO(&orig_r); SEL_ZERO(&orig_w); SEL_ZERO(&orig_e);
    if (urfds) orig_r = *urfds;
    if (uwfds) orig_w = *uwfds;
    if (uefds) orig_e = *uefds;

    struct task_struct *t = current_task;

    for (;;) {
        sel_fdset_t res_r, res_w, res_e;
        SEL_ZERO(&res_r); SEL_ZERO(&res_w); SEL_ZERO(&res_e);
        int ready = 0;

        for (int fd = 0; fd < nfds; fd++) {
            file_t *f = t->proc->fds->files[fd];

            if (urfds && SEL_ISSET(fd, &orig_r)) {
                if (f && f->node && (poll_vfs(f->node, VFS_POLLIN) & VFS_POLLIN)) {
                    SEL_SET(fd, &res_r); ready++;
                }
            }
            if (uwfds && SEL_ISSET(fd, &orig_w)) {
                if (f && f->node && (poll_vfs(f->node, VFS_POLLOUT) & VFS_POLLOUT)) {
                    SEL_SET(fd, &res_w); ready++;
                }
            }
            if (uefds && SEL_ISSET(fd, &orig_e)) {
                if (f && f->node) {
                    uint32_t rev = poll_vfs(f->node, VFS_POLLHUP | VFS_POLLERR);
                    if (rev & (VFS_POLLHUP | VFS_POLLERR)) {
                        SEL_SET(fd, &res_e); ready++;
                    }
                }
            }
        }

        if (ready > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0)) {
            if (urfds) copy_to_user(urfds, &res_r, sizeof(sel_fdset_t));
            if (uwfds) copy_to_user(uwfds, &res_w, sizeof(sel_fdset_t));
            if (uefds) copy_to_user(uefds, &res_e, sizeof(sel_fdset_t));
            return ready;
        }

        schedule();
    }
}

int sys_poll(struct syscall_frame *regs) {
    struct pollfd *fds_user   = (struct pollfd *)regs->ebx;
    int            nfds       = (int)regs->ecx;
    int            timeout_ms = (int)regs->edx;

    if (!current_task) return -1;
    if (nfds <= 0)     return 0;
    if ((uint32_t)nfds > UINT32_MAX / sizeof(struct pollfd)) return -1;
    if (!validate_user_ptr(fds_user, (uint32_t)nfds * sizeof(struct pollfd))) return -1;

    // Copy fds from user to kernel buffer for safe access across schedule()
    struct pollfd *fds = (struct pollfd *)kmalloc((uint32_t)nfds * sizeof(struct pollfd));
    if (!fds) return -1;
    if (copy_from_user(fds, fds_user, (uint32_t)nfds * sizeof(struct pollfd)) != 0) {
        kfree_heap(fds);
        return -1;
    }

#define TIMER_HZ_POLL 100
    int      infinite    = (timeout_ms < 0);
    int      nonblocking = (timeout_ms == 0);
    uint32_t deadline    = 0;
    if (!infinite && !nonblocking) {
        uint32_t ticks = (uint32_t)((timeout_ms + (1000 / TIMER_HZ_POLL) - 1) /
                                    (1000 / TIMER_HZ_POLL));
        deadline = timer_ticks_get() + ticks;
    }
#undef TIMER_HZ_POLL

    struct task_struct *t = current_task;

    for (;;) {
        int ready = 0;

        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_FD) { fds[i].revents = POLLNVAL; continue; }
            file_t *f = t->proc->fds->files[fd];
            if (!f || !f->node) { fds[i].revents = POLLNVAL; continue; }

            uint32_t events = 0;
            if (fds[i].events & POLLIN)  events |= VFS_POLLIN;
            if (fds[i].events & POLLOUT) events |= VFS_POLLOUT;

            uint32_t revents = (uint32_t)poll_vfs(f->node, events);

            short ev = 0;
            if (revents & VFS_POLLIN)  ev |= POLLIN;
            if (revents & VFS_POLLOUT) ev |= POLLOUT;
            if (revents & VFS_POLLHUP) ev |= POLLHUP;
            if (revents & VFS_POLLERR) ev |= POLLERR;
            if (revents & VFS_POLLNVAL) ev |= POLLNVAL;

            fds[i].revents = ev;
            if (ev) ready++;
        }

        if (ready > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0)) {
            copy_to_user(fds_user, fds, (uint32_t)nfds * sizeof(struct pollfd));
            kfree_heap(fds);
            return ready;
        }

        schedule();
    }
}
