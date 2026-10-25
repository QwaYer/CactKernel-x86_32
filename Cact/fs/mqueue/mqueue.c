#include "mqueue.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "sync.h"
#include "task.h"
#include "validate.h"
#include "ioctl_abi.h"
#include "helper.h"

// mqueue.c — the POSIX message-queue filesystem, mounted at /dev/mqueue.
//
// mq_open(3) opens (creating on demand) a *file* in this filesystem; the file
// is the queue.  Names, unlink-while-open lifetime and enumeration therefore
// come from the ordinary VFS/dirent machinery — the same shape as tmpfs — and
// mq_send/mq_receive are ioctls on the descriptor (a message carries a priority
// in both directions, which read()/write() cannot).  A queue is a
// priority-ordered list of kmalloc'd messages: the highest priority is received
// first, ties go FIFO, and mq_maxmsg bounds the list.
//
// Blocking uses the kernel's cooperative poll loop (mutex + sched_sleep_ticks,
// exactly like pipe/eventfd/epoll), bounded by the timer wheel for the timed
// forms and broken with -EINTR when a catchable signal is pending.  The
// descriptor's own O_NONBLOCK (set through fcntl, read here out of file_t)
// forces the non-blocking case.
//
// Lifetime mirrors tmpfs: a node's refcount is its dirent link plus its open
// descriptors, so an unlinked queue that is still open stays alive until the
// last close (POSIX), and the queue state is freed with the node.

extern void sched_sleep_ticks(uint32_t ticks);   // declared in syscall/process/proc.h

// Limits (the CACT_MQ_* names live in ioctl_abi.h, shared with userspace).
#define MQ_NAME_LEN        128                       // the dirent name buffer
#define MQ_MAXMSG_MAX      CACT_MQ_MAXMSG_LIMIT
#define MQ_MSGSIZE_MAX     CACT_MQ_MSGSIZE_LIMIT
#define MQ_BYTES_MAX       CACT_MQ_BYTES_LIMIT
#define MQ_DEFAULT_MAXMSG  CACT_MQ_DEFAULT_MAXMSG
#define MQ_DEFAULT_MSGSIZE CACT_MQ_DEFAULT_MSGSIZE
#define MQ_PRIO_MAX        CACT_MQ_PRIO_MAX

#define MQ_O_ACCMODE  0x0003
#define MQ_O_WRONLY   0x0001
#define MQ_O_NONBLOCK 0x0800

#ifndef EINTR
#define EINTR      4
#endif
#ifndef EBADF
#define EBADF      9
#endif
#ifndef EAGAIN
#define EAGAIN     11
#endif
#ifndef ENOMEM
#define ENOMEM     12
#endif
#ifndef EFAULT
#define EFAULT     14
#endif
#ifndef EBUSY
#define EBUSY      16
#endif
#ifndef EINVAL
#define EINVAL     22
#endif
#ifndef ENOTTY
#define ENOTTY     25
#endif
#ifndef EMSGSIZE
#define EMSGSIZE   90
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT  110
#endif

typedef struct mq_msg {
    struct mq_msg *next;
    uint32_t       prio;
    uint32_t       len;
    char           data[];      // `len` bytes
} mq_msg_t;

typedef struct mqueue {
    mutex_t   lock;
    int32_t   maxmsg;
    int32_t   msgsize;
    uint32_t  curmsgs;
    int       configured;       // CACT_MQCTL_INIT has fixed the limits
    mq_msg_t *head;             // highest priority first, FIFO within a priority
    uint32_t  notify_pid;       // 0 = no mq_notify registration
    uint32_t  notify_sig;       // the signal's mask bit
} mqueue_t;

typedef struct mq_dirent {
    char              name[MQ_NAME_LEN];
    vfs_node_t       *node;
    struct mq_dirent *next;
} mq_dirent_t;

typedef struct mq_dir {
    mq_dirent_t *children;
} mq_dir_t;

static uint32_t mqueue_inode_ctr = 1;
static uint32_t mqueue_bytes     = 0;   // budget shared by every queue's messages

static int  _mq_ioctl   (vfs_node_t*, void*, uint32_t, void*);
static int  _mq_poll    (vfs_node_t*, void*, uint32_t);
static void _mq_fopen   (vfs_node_t*, struct file*);
static void _mq_open    (vfs_node_t*);
static void _mq_close   (vfs_node_t*);
static vfs_node_t *_mq_walk   (vfs_node_t*, const char*);
static vfs_dirent_t *_mq_readdir(vfs_node_t*, uint32_t);
static void _mq_listdir (vfs_node_t*);
static int  _mq_create  (vfs_node_t*, const char*);
static int  _mq_delete  (vfs_node_t*, const char*);

static vfs_ops_t mqueue_ops = {
    .open    = _mq_open,
    .close   = _mq_close,
    .walk    = _mq_walk,
    .readdir = _mq_readdir,
    .listdir = _mq_listdir,
    .create  = _mq_create,
    .delete  = _mq_delete,
};

// Queue files use the per-open table so the handlers can see the descriptor
// (its flags), which the node-level table does not carry.
static vfs_file_ops_t mqueue_fops = {
    .open   = _mq_fopen,
    .ioctl  = _mq_ioctl,
    .poll   = _mq_poll,
};

static inline int _is_mqueue_file(vfs_node_t *node) {
    return node && node->fops == &mqueue_fops;
}

// ── byte budget ─────────────────────────────────────────────────────────

static int _mq_reserve_bytes(uint32_t n) {
    if (__sync_add_and_fetch(&mqueue_bytes, n) > MQ_BYTES_MAX) {
        __sync_sub_and_fetch(&mqueue_bytes, n);
        return 0;
    }
    return 1;
}

static void _mq_release_bytes(uint32_t n) {
    __sync_sub_and_fetch(&mqueue_bytes, n);
}

// ── queue core ──────────────────────────────────────────────────────────

// Allocate a message, reserving its bytes against the budget.  The caller
// fills data[] and enqueues it.  Returns NULL when the budget is exhausted.
static mq_msg_t *_mq_alloc_msg(uint32_t len) {
    uint32_t total = (uint32_t)sizeof(mq_msg_t) + len;
    if (!_mq_reserve_bytes(total)) return 0;
    mq_msg_t *m = (mq_msg_t *)kmalloc(total);
    if (!m) { _mq_release_bytes(total); return 0; }
    m->next = 0;
    m->prio = 0;
    m->len  = len;
    return m;
}

static void _mq_free_msg(mq_msg_t *m) {
    if (!m) return;
    _mq_release_bytes((uint32_t)sizeof(mq_msg_t) + m->len);
    kfree(m);
}

// Insert keeping the list ordered by priority (highest first) and FIFO within
// one priority: skip every message whose priority is >= the new one.
static void _mq_enqueue(mqueue_t *q, mq_msg_t *m) {
    mq_msg_t **pp = &q->head;
    while (*pp && (*pp)->prio >= m->prio) pp = &(*pp)->next;
    m->next = *pp;
    *pp = m;
}

static mq_msg_t *_mq_dequeue(mqueue_t *q) {
    mq_msg_t *m = q->head;
    if (m) {
        q->head = m->next;
        m->next = 0;
        q->curmsgs--;
    }
    return m;
}

static void _mq_free_node(vfs_node_t *node) {
    mqueue_t *q = (mqueue_t *)node->priv;
    if (q) {
        mq_msg_t *m = q->head;
        while (m) { mq_msg_t *n = m->next; _mq_free_msg(m); m = n; }
        kfree(q);
    }
    node->priv = 0;
    kfree(node);
}

// ── directory layer ─────────────────────────────────────────────────────

static mq_dirent_t *_find_dirent(mq_dir_t *dir, const char *name) {
    if (!dir) return 0;
    for (mq_dirent_t *d = dir->children; d; d = d->next)
        if (streq(d->name, name)) return d;
    return 0;
}

static int _add_dirent(mq_dir_t *dir, const char *name, vfs_node_t *node) {
    if (_find_dirent(dir, name)) return -1;
    mq_dirent_t *e = (mq_dirent_t *)kmalloc(sizeof(mq_dirent_t));
    if (!e) return -1;
    memset(e, 0, sizeof(mq_dirent_t));
    strlcpy(e->name, name, MQ_NAME_LEN);
    e->node = node;
    e->next = dir->children;
    dir->children = e;
    __sync_fetch_and_add(&node->refcount, 1);   // the link's own reference
    return 0;
}

static vfs_node_t *_mq_walk(vfs_node_t *dir, const char *name) {
    mq_dir_t *d = (mq_dir_t *)dir->priv;
    mq_dirent_t *e = _find_dirent(d, name);
    return e ? e->node : 0;
}

static vfs_dirent_t _mq_de;

static vfs_dirent_t *_mq_readdir(vfs_node_t *dir, uint32_t index) {
    mq_dir_t *d = (mq_dir_t *)dir->priv;
    if (!d) return 0;
    uint32_t i = 0;
    for (mq_dirent_t *e = d->children; e; e = e->next) {
        if (i++ == index) {
            strlcpy(_mq_de.name, e->name, 128);
            _mq_de.inode = e->node->inode;
            return &_mq_de;
        }
    }
    return 0;
}

static void _mq_listdir(vfs_node_t *dir) {
    mq_dir_t *d = (mq_dir_t *)dir->priv;
    if (!d) return;
    for (mq_dirent_t *e = d->children; e; e = e->next) {
        printk("  "); printk(e->name); printk("\n");
    }
}

static int _mq_create(vfs_node_t *dir, const char *name) {
    mq_dir_t *d = (mq_dir_t *)dir->priv;
    if (!d || dir->type != VFS_DIRECTORY) return -1;
    if (_find_dirent(d, name)) return -1;

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return -1;
    memset(node, 0, sizeof(vfs_node_t));

    mqueue_t *q = (mqueue_t *)kmalloc(sizeof(mqueue_t));
    if (!q) { kfree(node); return -1; }
    memset(q, 0, sizeof(mqueue_t));
    mutex_init(&q->lock);
    q->maxmsg  = MQ_DEFAULT_MAXMSG;
    q->msgsize = MQ_DEFAULT_MSGSIZE;
    q->configured = 0;

    strlcpy(node->name, name, 128);
    node->type     = VFS_FILE;
    node->inode    = mqueue_inode_ctr++;
    node->mode     = 0777;
    node->refcount = 0;                 // _add_dirent gives it its first link
    node->ops      = &mqueue_ops;
    node->fops     = &mqueue_fops;
    node->priv     = q;

    if (_add_dirent(d, name, node) != 0) { kfree(q); kfree(node); return -1; }
    return 0;
}

static int _mq_delete(vfs_node_t *dir, const char *name) {
    mq_dir_t *d = (mq_dir_t *)dir->priv;
    if (!d || dir->type != VFS_DIRECTORY) return -1;
    mq_dirent_t **pp = &d->children;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            mq_dirent_t *dead = *pp;
            vfs_node_t  *node = dead->node;
            *pp = dead->next;
            kfree(dead);
            if (__sync_fetch_and_sub(&node->refcount, 1) == 1)
                _mq_free_node(node);      // no links and no open descriptors
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

// ── node lifetime ───────────────────────────────────────────────────────

static void _mq_open(vfs_node_t *node) {
    if (node) __sync_fetch_and_add(&node->refcount, 1);
}

static void _mq_close(vfs_node_t *node) {
    if (!node || node->refcount == 0) return;
    if (__sync_fetch_and_sub(&node->refcount, 1) == 1) {
        // Only a queue file is heap-allocated per instance; the root directory
        // starts at refcount 1 and is never freed.
        if (node->type == VFS_FILE) _mq_free_node(node);
    }
}

static void _mq_fopen(vfs_node_t *node, struct file *f) {
    // The handlers need the descriptor's flags (access mode, O_NONBLOCK), and
    // file_t.priv is the only slot the file-aware wrappers pass through.
    if (_is_mqueue_file(node) && f) f->priv = f;
}

// ── the ioctls ──────────────────────────────────────────────────────────

// Common wait setup: O_NONBLOCK on the descriptor forces the poll case.
typedef struct mq_wait {
    int      infinite;
    int      poll;
    uint32_t deadline;
} mq_wait_t;

static void _mq_wait_init(mq_wait_t *w, file_t *f, int32_t timeout_ms) {
    int t = timeout_ms;
    if (f->flags & MQ_O_NONBLOCK) t = 0;
    w->infinite = (t < 0);
    w->poll     = (t == 0);
    w->deadline = 0;
    if (!w->infinite && !w->poll)
        w->deadline = timer_ticks_get() + ((uint32_t)t + 9u) / 10u;
}

// Returns the -errno to stop with, or 0 to keep waiting.
static int _mq_wait_step(mq_wait_t *w) {
    if (w->poll) return -EAGAIN;
    if (task_signal_pending_current()) return -EINTR;
    if (!w->infinite && (int32_t)(timer_ticks_get() - w->deadline) >= 0)
        return -ETIMEDOUT;
    sched_sleep_ticks(1);
    return 0;
}

static int _mq_do_init(mqueue_t *q, void *arg) {
    cact_mq_attr_t a;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    mutex_lock(&q->lock);
    int rc = 0;
    if (q->configured) rc = -EINVAL;              // limits are set once
    else if (a.mq_maxmsg <= 0 || a.mq_maxmsg > MQ_MAXMSG_MAX) rc = -EINVAL;
    else if (a.mq_msgsize <= 0 || a.mq_msgsize > MQ_MSGSIZE_MAX) rc = -EINVAL;
    else {
        q->maxmsg     = a.mq_maxmsg;
        q->msgsize    = a.mq_msgsize;
        q->configured = 1;
    }
    mutex_unlock(&q->lock);
    return rc;
}

static int _mq_do_send(mqueue_t *q, file_t *f, void *arg) {
    cact_mq_msg_t a;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (!a.msg_ptr || a.msg_len == 0) return -EINVAL;
    if (!validate_user_ptr(a.msg_ptr, a.msg_len)) return -EFAULT;
    if (a.msg_prio >= MQ_PRIO_MAX) return -EINVAL;
    if ((f->flags & MQ_O_ACCMODE) == 0) return -EBADF;   // O_RDONLY

    mq_wait_t w;
    _mq_wait_init(&w, f, a.timeout_ms);

    for (;;) {
        mutex_lock(&q->lock);
        if (a.msg_len > (uint32_t)q->msgsize) {
            mutex_unlock(&q->lock);
            return -EMSGSIZE;
        }
        if (q->curmsgs < (uint32_t)q->maxmsg) {
            mq_msg_t *m = _mq_alloc_msg(a.msg_len);
            if (!m) { mutex_unlock(&q->lock); return -ENOMEM; }
            m->prio = a.msg_prio;
            if (copy_from_user(m->data, a.msg_ptr, a.msg_len) != 0) {
                _mq_free_msg(m);
                mutex_unlock(&q->lock);
                return -EFAULT;
            }
            _mq_enqueue(q, m);
            q->curmsgs++;
            uint32_t npid = 0, nsig = 0;
            if (q->curmsgs == 1 && q->notify_pid) {   // empty -> non-empty
                npid = q->notify_pid;
                nsig = q->notify_sig;
                q->notify_pid = 0;
                q->notify_sig = 0;
            }
            mutex_unlock(&q->lock);
            if (npid) task_signal(npid, nsig);
            return 0;
        }
        mutex_unlock(&q->lock);
        int rc = _mq_wait_step(&w);
        if (rc) return rc;
    }
}

static int _mq_do_receive(mqueue_t *q, file_t *f, void *arg) {
    cact_mq_msg_t a;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (!a.msg_ptr || a.msg_len == 0) return -EINVAL;
    if (!validate_user_ptr(a.msg_ptr, a.msg_len)) return -EFAULT;
    if ((f->flags & MQ_O_ACCMODE) == MQ_O_WRONLY) return -EBADF;
    if (a.msg_len < (uint32_t)q->msgsize) return -EMSGSIZE;

    mq_wait_t w;
    _mq_wait_init(&w, f, a.timeout_ms);

    for (;;) {
        mutex_lock(&q->lock);
        mq_msg_t *m = _mq_dequeue(q);
        if (m) {
            uint32_t len = m->len, prio = m->prio;
            int rc = copy_to_user(a.msg_ptr, m->data, len);
            _mq_free_msg(m);
            mutex_unlock(&q->lock);
            if (rc != 0) return -EFAULT;
            a.msg_len  = len;
            a.msg_prio = prio;
            return copy_to_user(arg, &a, sizeof(a));
        }
        mutex_unlock(&q->lock);
        int rc = _mq_wait_step(&w);
        if (rc) return rc;
    }
}

static int _mq_do_getattr(mqueue_t *q, file_t *f, void *arg) {
    cact_mq_attr_t out;
    if (!arg) return -EINVAL;
    memset(&out, 0, sizeof(out));
    mutex_lock(&q->lock);
    out.mq_flags   = (f->flags & MQ_O_NONBLOCK) ? MQ_O_NONBLOCK : 0;
    out.mq_maxmsg  = q->maxmsg;
    out.mq_msgsize = q->msgsize;
    out.mq_curmsgs = (int32_t)q->curmsgs;
    mutex_unlock(&q->lock);
    return copy_to_user(arg, &out, sizeof(out));
}

static int _mq_do_setattr(mqueue_t *q, file_t *f, void *arg) {
    cact_mq_attr_t a, old;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    memset(&old, 0, sizeof(old));
    mutex_lock(&q->lock);
    old.mq_flags   = (f->flags & MQ_O_NONBLOCK) ? MQ_O_NONBLOCK : 0;
    old.mq_maxmsg  = q->maxmsg;
    old.mq_msgsize = q->msgsize;
    old.mq_curmsgs = (int32_t)q->curmsgs;
    // POSIX: only mq_flags may change (and only O_NONBLOCK in practice).
    if (a.mq_flags & MQ_O_NONBLOCK) f->flags |= MQ_O_NONBLOCK;
    else                            f->flags &= ~MQ_O_NONBLOCK;
    mutex_unlock(&q->lock);
    return copy_to_user(arg, &old, sizeof(old));
}

static int _mq_do_notify(mqueue_t *q, void *arg) {
    cact_mq_notify_arg_t a;
    if (!arg) return -EINVAL;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    mutex_lock(&q->lock);
    if (a.signo == 0) {                       // SIGEV_NONE / NULL sevp
        q->notify_pid = 0;
        q->notify_sig = 0;
        mutex_unlock(&q->lock);
        return 0;
    }
    uint32_t me = current_task ? current_task->pid : 0;
    if (!me) { mutex_unlock(&q->lock); return -EINVAL; }
    if (q->notify_pid) { mutex_unlock(&q->lock); return -EBUSY; }
    if (q->curmsgs > 0) {
        // POSIX: registering on a non-empty queue notifies at once.
        mutex_unlock(&q->lock);
        task_signal(me, a.signo);
        return 0;
    }
    q->notify_pid = me;
    q->notify_sig = a.signo;
    mutex_unlock(&q->lock);
    return 0;
}

static int _mq_ioctl(vfs_node_t *node, void *priv, uint32_t cmd, void *arg) {
    mqueue_t *q = (mqueue_t *)node->priv;
    file_t   *f = (file_t *)priv;
    if (!q || !f) return -EINVAL;
    switch (cmd) {
    case CACT_MQCTL_INIT:    return _mq_do_init(q, arg);
    case CACT_MQCTL_SEND:    return _mq_do_send(q, f, arg);
    case CACT_MQCTL_RECEIVE: return _mq_do_receive(q, f, arg);
    case CACT_MQCTL_GETATTR: return _mq_do_getattr(q, f, arg);
    case CACT_MQCTL_SETATTR: return _mq_do_setattr(q, f, arg);
    case CACT_MQCTL_NOTIFY:  return _mq_do_notify(q, arg);
    default:                 return -ENOTTY;
    }
}

static int _mq_poll(vfs_node_t *node, void *priv, uint32_t events) {
    (void)priv;
    mqueue_t *q = (mqueue_t *)node->priv;
    if (!q) return VFS_POLLERR;
    uint32_t revents = 0;
    mutex_lock(&q->lock);
    if ((events & VFS_POLLIN) && q->curmsgs > 0) revents |= VFS_POLLIN;
    if ((events & VFS_POLLOUT) && q->curmsgs < (uint32_t)q->maxmsg)
        revents |= VFS_POLLOUT;
    mutex_unlock(&q->lock);
    return (int)revents;
}

// ── construction ────────────────────────────────────────────────────────

vfs_node_t *mqueue_create_root(const char *name) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    memset(node, 0, sizeof(vfs_node_t));

    mq_dir_t *d = (mq_dir_t *)kmalloc(sizeof(mq_dir_t));
    if (!d) { kfree(node); return 0; }
    memset(d, 0, sizeof(mq_dir_t));

    strlcpy(node->name, name ? name : "mqueue", 128);
    node->type     = VFS_DIRECTORY;
    node->inode    = mqueue_inode_ctr++;
    node->mode     = 0777;
    node->refcount = 1;                 // the mount's own hold; never freed
    node->ops      = &mqueue_ops;
    node->priv     = d;
    return node;
}
