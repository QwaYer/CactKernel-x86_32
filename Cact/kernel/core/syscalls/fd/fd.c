#include "fd.h"
#include "validate.h"
#include "helper.h"
#include "resolve.h"
#include "pipe.h"
#include "path/path.h"
#include "file/file.h"

// Flags that may be changed via fcntl F_SETFL
#define SETFL_MASK  (0x0800 | 0x0400)
#define OPEN_ACCMODE 0x0003
#define OPEN_WRONLY  0x0001
#define OPEN_RDWR    0x0002
#define OPEN_CREAT   0x0040
#define OPEN_TRUNC   0x0200

// Open a file by path, allocate an fd starting from 3 (0-2 are stdin/stdout/stderr)
int sys_open(char* name, int flags) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    vfs_node_t* node = _resolve_path(name);
    if (!node) {
        if ((flags & OPEN_CREAT) == 0) {
            return -1;
        }

        if (sys_create(name) != 0) {
            return -1;
        }

        node = _resolve_path(name);
        if (!node) {
            return -1;
        }
    }

    // Permission check based on requested access mode (bits 0-1 of flags)
    {
        uint32_t need = 0;
        uint32_t acc  = (uint32_t)flags & OPEN_ACCMODE;
        if (acc == 0 || acc == 2) need |= VFS_PERM_READ;
        if (acc == 1 || acc == 2) need |= VFS_PERM_WRITE;
        if (vfs_check_perm(node, need) < 0) return -1;
    }

    // Find a free fd slot (3..MAX_FD-1)
    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->fds->fd_table[i]) {
            current_task->fds->fd_table[i]   = node;
            current_task->fds->fd_offset[i]  = 0;
            current_task->fds->fd_flags[i]   = (uint32_t)flags;
            current_task->fds->fd_cloexec[i] = 0;
            open_vfs(node);   // increment VFS refcount

            // Truncate regular files on open(O_TRUNC) when opened writable.
            if ((flags & OPEN_TRUNC) &&
                (((flags & OPEN_ACCMODE) == OPEN_WRONLY) ||
                 ((flags & OPEN_ACCMODE) == OPEN_RDWR))) {
                if (sys_ftruncate(i, 0) != 0) {
                    current_task->fds->fd_table[i]   = 0;
                    current_task->fds->fd_offset[i]  = 0;
                    current_task->fds->fd_flags[i]   = 0;
                    current_task->fds->fd_cloexec[i] = 0;
                    close_vfs(node);
                    return -1;
                }
            }

            return i;
        }
    }
    return -1;
}

// Read from a file descriptor at the current offset, advancing it
int sys_read(int fd, char* buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;
    if (!current_task)                 return -1;
    if (fd < 0 || fd >= MAX_FD)        return -1;

    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;
    int ret = read_vfs(node, current_task->fds->fd_offset[fd], size, buf);
    if (ret > 0)
        current_task->fds->fd_offset[fd] += (uint32_t)ret;
    return ret;
}

// Write to a file descriptor at the current offset, advancing it
int sys_write(int fd, char* buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;
    if (!current_task)                 return -1;
    if (fd < 0 || fd >= MAX_FD)        return -1;
    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;
    int ret = write_vfs(node, current_task->fds->fd_offset[fd], size, buf);
    if (ret > 0)
        current_task->fds->fd_offset[fd] += (uint32_t)ret;
    return ret;
}

// Close a file descriptor, releasing the VFS reference
int sys_close(int fd) {
    if (!current_task)          return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;
    current_task->fds->fd_table[fd]   = 0;
    current_task->fds->fd_offset[fd]  = 0;
    current_task->fds->fd_flags[fd]   = 0;
    current_task->fds->fd_cloexec[fd] = 0;
    close_vfs(node);   // decrement VFS refcount
    return 0;
}

// Reposition the file offset for a descriptor (SEEK_SET, SEEK_CUR, SEEK_END)
int sys_lseek(struct syscall_frame* regs) {
    int fd     = (int)regs->ebx;
    int offset = (int)regs->ecx;
    int whence = (int)regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;

    uint32_t new_off;
    switch (whence) {
        case SEEK_SET:
            if (offset < 0) return -1;
            new_off = (uint32_t)offset;
            break;
        case SEEK_CUR: {
            int cur = (int)current_task->fds->fd_offset[fd] + offset;
            if (cur < 0) return -1;
            new_off = (uint32_t)cur;
            break;
        }
        case SEEK_END: {
            int end = (int)node->size + offset;
            if (end < 0) return -1;
            new_off = (uint32_t)end;
            break;
        }
        default:
            return -1;
    }
    current_task->fds->fd_offset[fd] = new_off;
    return (int)new_off;
}

// I/O control: terminal window size (TIOCGWINSZ/TIOCSWINSZ) or device-specific ioctl
int sys_ioctl(struct syscall_frame* regs) {
    int      fd  = (int)regs->ebx;
    uint32_t cmd = regs->ecx;
    void*    arg = (void*)regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    // Terminal window size queries
    if (cmd == TIOCGWINSZ) {
        if (!arg || !validate_user_ptr(arg, sizeof(struct winsize))) return -1;
        struct winsize* ws = (struct winsize*)arg;
        ws->ws_row    = terminal_winsize.ws_row;
        ws->ws_col    = terminal_winsize.ws_col;
        ws->ws_xpixel = terminal_winsize.ws_xpixel;
        ws->ws_ypixel = terminal_winsize.ws_ypixel;
        return 0;
    }

    if (cmd == TIOCSWINSZ) {
        if (!arg || !validate_user_ptr(arg, sizeof(struct winsize))) return -1;
        struct winsize* ws = (struct winsize*)arg;
        terminal_winsize.ws_row    = ws->ws_row;
        terminal_winsize.ws_col    = ws->ws_col;
        terminal_winsize.ws_xpixel = ws->ws_xpixel;
        terminal_winsize.ws_ypixel = ws->ws_ypixel;
        if (terminal_fg_pid)
            task_signal(terminal_fg_pid, SIGWINCH);   // notify terminal
        return 0;
    }

    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;

    if (arg && !validate_user_ptr(arg, 1)) return -1;

    return ioctl_vfs(node, cmd, arg);
}

// File descriptor control: dup, get/set CLOEXEC, get/set flags
int sys_fcntl(int fd, int cmd, int arg) {
    if (!current_task)          return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;

    switch (cmd) {

    // F_DUPFD: duplicate fd starting from 'arg'
    case 0: {
        if (arg < 0 || arg >= MAX_FD) return -1;
        for (int i = arg; i < MAX_FD; i++) {
            if (!current_task->fds->fd_table[i]) {
                current_task->fds->fd_table[i]   = node;
                current_task->fds->fd_offset[i]  = current_task->fds->fd_offset[fd];
                current_task->fds->fd_flags[i]   = current_task->fds->fd_flags[fd];
                current_task->fds->fd_cloexec[i] = 0;
                open_vfs(node);
                return i;
            }
        }
        return -1;
    }

    // F_GETFD: get close-on-exec flag
    case 1:
        return (int)current_task->fds->fd_cloexec[fd];

    // F_SETFD: set close-on-exec flag
    case 2:
        current_task->fds->fd_cloexec[fd] = (arg & 1) ? 1 : 0;
        return 0;

    // F_GETFL: get file status flags
    case 3:
        return (int)current_task->fds->fd_flags[fd];

    // F_SETFL: set file status flags (only O_NONBLOCK and O_APPEND allowed)
    case 4: {
        uint32_t new_flags = (current_task->fds->fd_flags[fd] & ~(uint32_t)SETFL_MASK)
                           | ((uint32_t)arg & SETFL_MASK);
        current_task->fds->fd_flags[fd] = new_flags;

        // Propagate O_NONBLOCK to pipe if applicable
        if (node->type == VFS_PIPE && node->priv) {
            pipe_t* p = (pipe_t*)node->priv;
            if (new_flags & 0x0800)
                p->flags |=  O_NONBLOCK;
            else
                p->flags &= ~O_NONBLOCK;
        }
        return 0;
    }

    default:
        return -1;
    }
}

// Duplicate a file descriptor (kernel picks the lowest available number)
int sys_dup(int oldfd) {
    if (!current_task) return -1;
    if (oldfd < 0 || oldfd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fds->fd_table[oldfd];
    if (!node) return -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (!current_task->fds->fd_table[i]) {
            open_vfs(node);
            current_task->fds->fd_table[i]   = node;
            current_task->fds->fd_offset[i]  = current_task->fds->fd_offset[oldfd];
            current_task->fds->fd_flags[i]   = current_task->fds->fd_flags[oldfd];
            current_task->fds->fd_cloexec[i] = 0;
            return i;
        }
    }
    return -1;
}

// Duplicate oldfd to newfd, closing newfd first if necessary
int sys_dup2(struct syscall_frame* regs) {
    int oldfd = (int)regs->ebx;
    int newfd = (int)regs->ecx;

    if (!current_task) return -1;
    if (oldfd < 0 || oldfd >= MAX_FD) return -1;
    if (newfd < 0 || newfd >= MAX_FD) return -1;

    // POSIX: dup2(fd, fd) is a no-op
    if (oldfd == newfd) return newfd;

    struct vfs_node* node = current_task->fds->fd_table[oldfd];
    if (!node) return -1;

    // Increment refcount before installing to avoid transient UAF
    open_vfs(node);

    struct vfs_node* old_newnode = current_task->fds->fd_table[newfd];
    current_task->fds->fd_table[newfd]   = node;
    current_task->fds->fd_offset[newfd]  = current_task->fds->fd_offset[oldfd];
    current_task->fds->fd_flags[newfd]   = current_task->fds->fd_flags[oldfd];
    current_task->fds->fd_cloexec[newfd] = 0;

    if (old_newnode)
        close_vfs(old_newnode);   // release old occupant of newfd

    return newfd;
}

// Create a pipe pair, storing the fds in user_fds[2]
int sys_pipe(struct syscall_frame* regs) {
    int* user_fds = (int*)regs->ebx;
    if (!validate_user_ptr(user_fds, sizeof(int) * 2)) return -1;
    if (!current_task) return -1;

    struct vfs_node* pipefd[2];
    if (pipe_create(pipefd, 0) != 0) return -1;

    int rfd = -1, wfd = -1;
    for (int i = 3; i < MAX_FD && (rfd < 0 || wfd < 0); i++) {
        if (!current_task->fds->fd_table[i]) {
            if (rfd < 0) rfd = i;
            else         wfd = i;
        }
    }

    if (rfd < 0 || wfd < 0) {
        close_vfs(pipefd[0]);
        close_vfs(pipefd[1]);
        return -1;
    }

    current_task->fds->fd_table[rfd]   = pipefd[0];
    current_task->fds->fd_table[wfd]   = pipefd[1];
    current_task->fds->fd_offset[rfd]  = 0;
    current_task->fds->fd_offset[wfd]  = 0;
    current_task->fds->fd_flags[rfd]   = 0;
    current_task->fds->fd_flags[wfd]   = 0;
    current_task->fds->fd_cloexec[rfd] = 0;
    current_task->fds->fd_cloexec[wfd] = 0;

    user_fds[0] = rfd;
    user_fds[1] = wfd;
    return 0;
}

// POSIX select(): wait for readiness on multiple fds with optional timeout
int sys_select(struct syscall_frame* regs) {
    sel_args_t* ua = (sel_args_t*)regs->ebx;
    if (!validate_user_ptr(ua, sizeof(sel_args_t))) return -1;
    if (!current_task) return -1;

    int              nfds  = ua->nfds;
    sel_fdset_t*     urfds = ua->readfds;
    sel_fdset_t*     uwfds = ua->writefds;
    sel_fdset_t*     uefds = ua->exceptfds;
    struct timeval_fd* utv = ua->timeout;

    if (nfds < 0 || nfds > MAX_FD)                                     return -1;
    if (urfds && !validate_user_ptr(urfds, sizeof(sel_fdset_t)))        return -1;
    if (uwfds && !validate_user_ptr(uwfds, sizeof(sel_fdset_t)))        return -1;
    if (uefds && !validate_user_ptr(uefds, sizeof(sel_fdset_t)))        return -1;
    if (utv   && !validate_user_ptr(utv,   sizeof(struct timeval_fd)))  return -1;

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

    struct task_struct* t = current_task;

    for (;;) {
        sel_fdset_t res_r, res_w, res_e;
        SEL_ZERO(&res_r); SEL_ZERO(&res_w); SEL_ZERO(&res_e);
        int ready = 0;

        for (int fd = 0; fd < nfds; fd++) {
            vfs_node_t* node = t->fds->fd_table[fd];

            if (urfds && SEL_ISSET(fd, &orig_r)) {
                if (node && fd_read_ready(node)) { SEL_SET(fd, &res_r); ready++; }
            }
            if (uwfds && SEL_ISSET(fd, &orig_w)) {
                if (node && fd_write_ready(node)) { SEL_SET(fd, &res_w); ready++; }
            }
            if (uefds && SEL_ISSET(fd, &orig_e)) {
                if (node && node->type == VFS_PIPE) {
                    pipe_t* p = (pipe_t*)node->priv;
                    if (p && !p->write_open) { SEL_SET(fd, &res_e); ready++; }
                }
            }
        }

        if (ready > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0)) {
            if (urfds) *urfds = res_r;
            if (uwfds) *uwfds = res_w;
            if (uefds) *uefds = res_e;
            return ready;
        }

        schedule();   // yield CPU, will be woken by timer or IRQ
    }
}

// POSIX poll(): similar to select but uses pollfd array
int sys_poll(struct syscall_frame* regs) {
    struct pollfd* fds        = (struct pollfd*)regs->ebx;
    int            nfds       = (int)regs->ecx;
    int            timeout_ms = (int)regs->edx;

    if (!current_task) return -1;
    if (nfds <= 0)     return 0;
    if (!validate_user_ptr(fds, (uint32_t)nfds * sizeof(struct pollfd))) return -1;

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

    struct task_struct* t = current_task;

    for (;;) {
        int ready = 0;

        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_FD) { fds[i].revents = POLLNVAL; continue; }
            vfs_node_t* node = t->fds->fd_table[fd];
            if (!node)                  { fds[i].revents = POLLNVAL; continue; }

            short ev = 0;
            if ((fds[i].events & POLLIN)  && fd_read_ready(node))  ev |= POLLIN;
            if ((fds[i].events & POLLOUT) && fd_write_ready(node)) ev |= POLLOUT;

            // Pipes: report hangup/error regardless of requested events
            if (node->type == VFS_PIPE) {
                pipe_t* pp = (pipe_t*)node->priv;
                if (pp) {
                    int is_wr = (int)node->inode;
                    if (!is_wr && !pp->write_open && pp->len == 0)
                        ev |= POLLHUP;     // read end, writer gone, no data
                    if (is_wr && !pp->read_open)
                        ev |= POLLERR;     // write end, reader gone
                }
            }

            fds[i].revents = ev;
            if (ev) ready++;
        }

        if (ready > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0))
            return ready;

        schedule();
    }
}