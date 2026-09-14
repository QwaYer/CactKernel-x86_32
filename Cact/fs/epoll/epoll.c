#include "epoll.h"
#include "task.h"
#include "sync.h"
#include "klib.h"
#include "kernel.h"
#include "helper.h"
#include "ioctl_abi.h"
#include "validate.h"

// epoll.c — epoll(2) VFS node.
//
// Level-triggered epoll implemented on top of the kernel's existing poll
// machinery (poll_vfs + the schedule() deadline loop), exactly like sys_poll
// in fd_mux.c.  There are no per-node wait queues in this kernel, so
// EPOLL_WAIT re-probes every registered fd each time it is scheduled.
//
// Each registration keeps a file_t reference, so a monitored file cannot
// vanish while registered; EPOLL_CTL_DEL and epoll teardown drop it.  (An fd
// closed without EPOLL_CTL_DEL therefore stays registered until removed.)

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef EBADF
#define EBADF 9
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

#define EPOLL_MAX_ENTRIES 128

/* Linux epoll event numbers (equal to POLL numbers for the subset we use). */
#define EPOLLIN   0x001
#define EPOLLOUT  0x004
#define EPOLLERR  0x008
#define EPOLLHUP  0x010
#define EPOLLET   (1u << 31)

typedef struct epoll_entry {
    file_t  *file;      /* holds a reference */
    uint32_t events;    /* interest mask */
    uint64_t data;      /* opaque user data */
    uint32_t in_use;
} epoll_entry_t;

typedef struct epoll_state {
    epoll_entry_t entries[EPOLL_MAX_ENTRIES];
    uint32_t      nentries;
    uint32_t      flags;   /* CACT_EPOLL_CLOEXEC */
    mutex_t       lock;
} epoll_state_t;

/* Wire event record: matches <sys/epoll.h> struct epoll_event (packed). */
typedef struct epoll_event_wire {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) epoll_event_wire_t;

static int _epoll_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _epoll_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _epoll_open(vfs_node_t *node);
static void _epoll_close(vfs_node_t *node);
static int _epoll_poll(vfs_node_t *node, uint32_t events);
static int _epoll_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);

static vfs_ops_t epoll_ops = {
    .read  = _epoll_read,
    .write = _epoll_write,
    .open  = _epoll_open,
    .close = _epoll_close,
    .poll  = _epoll_poll,
    .ioctl = _epoll_ioctl,
};

static inline int _is_epoll(vfs_node_t *node) {
    return node && node->ops == &epoll_ops;
}

static epoll_state_t *_state(vfs_node_t *node) {
    if (!_is_epoll(node)) return 0;
    return (epoll_state_t *)node->priv;
}

static file_t *_resolve_fd(int fd) {
    if (!current_task || !current_task->proc || !current_task->proc->fds) return 0;
    if (fd < 0 || fd >= MAX_FD) return 0;
    return current_task->proc->fds->files[fd];
}

/* Level-triggered readiness of one monitored file.  The interest mask uses
 * EPOLLIN/EPOLLOUT, which share bit values with the kernel's VFS_POLL*. */
static uint32_t _ready_of(file_t *f, uint32_t interest) {
    if (!f || !f->node) return EPOLLERR;
    int r = poll_vfs(f->node, interest & (EPOLLIN | EPOLLOUT));
    uint32_t rev = (r < 0) ? 0 : (uint32_t)r;
    uint32_t out = 0;
    if (rev & EPOLLIN)  out |= EPOLLIN;
    if (rev & EPOLLOUT) out |= EPOLLOUT;
    if (rev & EPOLLERR) out |= EPOLLERR;
    if (rev & EPOLLHUP) out |= EPOLLHUP;
    return out;
}

static int _epoll_ctl_add(epoll_state_t *s, int tfd, uint32_t events, uint64_t data,
                          vfs_node_t *self_node) {
    file_t *f = _resolve_fd(tfd);
    if (!f || !f->node) return -EBADF;
    if (f->node == self_node) return -EINVAL;

    mutex_lock(&s->lock);
    for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES; i++) {
        if (!s->entries[i].in_use) continue;
        if (s->entries[i].file == f) {
            mutex_unlock(&s->lock);
            return -EEXIST;
        }
    }
    if (s->nentries >= EPOLL_MAX_ENTRIES) {
        mutex_unlock(&s->lock);
        return -ENOSPC;
    }
    for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES; i++) {
        if (!s->entries[i].in_use) {
            file_ref(f);
            s->entries[i].file   = f;
            s->entries[i].events = events;
            s->entries[i].data   = data;
            s->entries[i].in_use = 1;
            s->nentries++;
            mutex_unlock(&s->lock);
            return 0;
        }
    }
    mutex_unlock(&s->lock);
    return -ENOSPC;
}

static int _epoll_ctl_mod(epoll_state_t *s, int tfd, uint32_t events, uint64_t data) {
    file_t *f = _resolve_fd(tfd);
    if (!f || !f->node) return -EBADF;

    mutex_lock(&s->lock);
    for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES; i++) {
        if (!s->entries[i].in_use) continue;
        if (s->entries[i].file == f) {
            s->entries[i].events = events;
            s->entries[i].data   = data;
            mutex_unlock(&s->lock);
            return 0;
        }
    }
    mutex_unlock(&s->lock);
    return -ENOENT;
}

static int _epoll_ctl_del(epoll_state_t *s, int tfd) {
    file_t *f = _resolve_fd(tfd);
    if (!f || !f->node) return -EBADF;

    mutex_lock(&s->lock);
    for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES; i++) {
        if (!s->entries[i].in_use) continue;
        if (s->entries[i].file == f) {
            file_unref(s->entries[i].file);
            s->entries[i].in_use = 0;
            s->entries[i].file   = 0;
            s->nentries--;
            mutex_unlock(&s->lock);
            return 0;
        }
    }
    mutex_unlock(&s->lock);
    return -ENOENT;
}

static int _epoll_ctl(vfs_node_t *node, void *arg) {
    if (!arg) return -EINVAL;
    cact_epoll_ctl_arg_t a;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    epoll_state_t *s = _state(node);
    if (!s) return -ENOTTY;

    switch (a.op) {
    case CACT_EPOLL_CTL_ADD: return _epoll_ctl_add(s, a.fd, a.events, a.data, node);
    case CACT_EPOLL_CTL_MOD: return _epoll_ctl_mod(s, a.fd, a.events, a.data);
    case CACT_EPOLL_CTL_DEL: return _epoll_ctl_del(s, a.fd);
    default:                 return -EINVAL;
    }
}

/* Collect currently-ready events (up to max) into out[]; returns the count.
 * Takes a transient file_t reference on each snapshot entry so a concurrent
 * EPOLL_CTL_DEL / epoll teardown cannot free a file while it is polled. */
static int _epoll_collect(epoll_state_t *s, epoll_event_wire_t *out, int max) {
    file_t  *snap[EPOLL_MAX_ENTRIES];
    uint32_t ev[EPOLL_MAX_ENTRIES];
    uint64_t dt[EPOLL_MAX_ENTRIES];
    int n = 0;

    mutex_lock(&s->lock);
    for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES && n < EPOLL_MAX_ENTRIES; i++) {
        if (!s->entries[i].in_use) continue;
        snap[n] = file_ref(s->entries[i].file);
        ev[n]   = s->entries[i].events;
        dt[n]   = s->entries[i].data;
        n++;
    }
    mutex_unlock(&s->lock);

    int ready = 0;
    for (int i = 0; i < n && ready < max; i++) {
        uint32_t e = _ready_of(snap[i], ev[i]);
        if (e == 0) continue;
        out[ready].events = e;
        out[ready].data   = dt[i];
        ready++;
    }
    for (int i = 0; i < n; i++) file_unref(snap[i]);
    return ready;
}

static int _epoll_wait(vfs_node_t *node, void *arg) {
    if (!arg) return -EINVAL;
    cact_epoll_wait_arg_t a;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (!a.events || a.maxevents < 1) return -EINVAL;
    if (a.maxevents > EPOLL_MAX_ENTRIES) a.maxevents = EPOLL_MAX_ENTRIES;

    epoll_state_t *s = _state(node);
    if (!s) return -ENOTTY;

    if (!validate_user_ptr(a.events, a.maxevents * sizeof(epoll_event_wire_t)))
        return -EFAULT;

    int      infinite    = (a.timeout_ms < 0);
    int      nonblocking = (a.timeout_ms == 0);
    uint32_t deadline    = 0;
    if (!infinite && !nonblocking) {
        uint32_t ticks = (uint32_t)((a.timeout_ms + 9) / 10);
        deadline = timer_ticks_get() + ticks;
    }

    epoll_event_wire_t *out =
        (epoll_event_wire_t *)kmalloc(a.maxevents * sizeof(epoll_event_wire_t));
    if (!out) return -ENOMEM;

    for (;;) {
        int got = _epoll_collect(s, out, (int)a.maxevents);
        if (got > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0)) {
            int rc = copy_to_user(a.events, out,
                                  (uint32_t)got * sizeof(epoll_event_wire_t));
            kfree(out);
            if (rc != 0) return -EFAULT;
            return got;
        }
        schedule();
        if (!_is_epoll(node)) {
            kfree(out);
            return -1;
        }
    }
}

static int _epoll_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    switch (cmd) {
    case CACT_EPOLL_CTL:  return _epoll_ctl(node, arg);
    case CACT_EPOLL_WAIT: return _epoll_wait(node, arg);
    default:              return -ENOTTY;
    }
}

static int _epoll_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node; (void)off; (void)size; (void)buf;
    return -EINVAL;
}

static int _epoll_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node; (void)off; (void)size; (void)buf;
    return -EINVAL;
}

static void _epoll_open(vfs_node_t *node) {
    if (_is_epoll(node)) node->refcount++;
}

static void _epoll_close(vfs_node_t *node) {
    if (!_is_epoll(node)) return;
    if (node->refcount > 0) node->refcount--;
    if (node->refcount == 0) {
        epoll_state_t *s = (epoll_state_t *)node->priv;
        if (s) {
            for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES; i++) {
                if (s->entries[i].in_use) {
                    file_unref(s->entries[i].file);
                    s->entries[i].in_use = 0;
                    s->entries[i].file   = 0;
                }
            }
            kfree(s);
        }
        node->priv = 0;
        kfree(node);
    }
}

/* poll(2) on the epoll fd itself: readable when any entry is ready.
 * Poll ops are non-blocking probes; no other path takes the state lock while
 * holding a target node lock, so iterating under the state lock is safe. */
static int _epoll_poll(vfs_node_t *node, uint32_t events) {
    epoll_state_t *s = _state(node);
    if (!s) return VFS_POLLERR;
    uint32_t revents = 0;
    if (events & VFS_POLLIN) {
        mutex_lock(&s->lock);
        for (uint32_t i = 0; i < EPOLL_MAX_ENTRIES; i++) {
            if (!s->entries[i].in_use) continue;
            if (_ready_of(s->entries[i].file, s->entries[i].events) != 0) {
                revents |= VFS_POLLIN;
                break;
            }
        }
        mutex_unlock(&s->lock);
    }
    return (int)revents;
}

vfs_node_t *epoll_create_vnode(uint32_t flags) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        pr_err("  %-11s : cannot allocate node\n", "epoll");
        return 0;
    }
    memset(node, 0, sizeof(vfs_node_t));

    epoll_state_t *s = (epoll_state_t *)kmalloc(sizeof(epoll_state_t));
    if (!s) {
        kfree(node);
        pr_err("  %-11s : cannot allocate state\n", "epoll");
        return 0;
    }
    memset(s, 0, sizeof(epoll_state_t));
    s->flags = flags & CACT_EPOLL_CLOEXEC;
    mutex_init(&s->lock);

    strlcpy(node->name, "epoll", 128);
    node->type     = VFS_FILE;
    node->inode    = 0;
    node->refcount = 0;
    node->ops      = &epoll_ops;
    node->priv     = s;
    return node;
}
