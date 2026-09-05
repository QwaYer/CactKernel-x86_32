#include "fd.h"
#include "validate.h"
#include "helper.h"
#include "pipe.h"
#include "kernel.h"   // terminal_winsize

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

    // poll(NULL, 0, timeout) — чистый sleep (nfds == 0).
    if (nfds <= 0) {
        if (timeout_ms == 0) return 0;
        if (timeout_ms < 0) { for (;;) schedule(); }
        uint32_t ticks = (uint32_t)((timeout_ms + (1000 / 100) - 1) / (1000 / 100));
        uint32_t deadline = timer_ticks_get() + ticks;
        while ((int32_t)(timer_ticks_get() - deadline) < 0) schedule();
        return 0;
    }
    if ((uint32_t)nfds > UINT32_MAX / sizeof(struct pollfd)) return -1;
    if (!validate_user_ptr(fds_user, (uint32_t)nfds * sizeof(struct pollfd))) return -1;

    // Copy fds from user to kernel buffer for safe access across schedule()
    struct pollfd *fds = (struct pollfd *)kmalloc((uint32_t)nfds * sizeof(struct pollfd));
    if (!fds) return -1;
    if (copy_from_user(fds, fds_user, (uint32_t)nfds * sizeof(struct pollfd)) != 0) {
        kfree(fds);
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
            kfree(fds);
            return ready;
        }

        schedule();
    }
}
