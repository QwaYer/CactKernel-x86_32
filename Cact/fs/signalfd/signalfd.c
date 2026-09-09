#include "signalfd.h"
#include "task.h"
#include "sync.h"
#include "klib.h"
#include "helper.h"
#include "ioctl_abi.h"
#include "validate.h"

// signalfd.c — signalfd(2) VFS node.
//
// The node subscribes to a set of kernel signal bits (0..12).  read(2)
// returns one 128-byte signalfd_siginfo per pending, currently *blocked*
// signal that is in the mask, and clears that bit from the process pending
// set.  Signals are never stolen from normal delivery: unblocked signals are
// handled at the syscall-return boundary, only blocked ones accumulate and
// can be consumed here.  poll(2) reports POLLIN while such a signal is
// pending.  This matches the way libwayland uses signalfd (sigprocmask
// SIG_BLOCK first, then signalfd()).

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

#define SIGNALFD_INFO_SIZE 128

typedef struct signalfd_state {
    uint32_t mask;    /* subscribed kernel signal bits */
    uint32_t flags;   /* CACT_SFD_NONBLOCK            */
    mutex_t  lock;
} signalfd_state_t;

static int _signalfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _signalfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _signalfd_open(vfs_node_t *node);
static void _signalfd_close(vfs_node_t *node);
static int _signalfd_poll(vfs_node_t *node, uint32_t events);
static int _signalfd_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);

static vfs_ops_t signalfd_ops = {
    .read  = _signalfd_read,
    .write = _signalfd_write,
    .open  = _signalfd_open,
    .close = _signalfd_close,
    .poll  = _signalfd_poll,
    .ioctl = _signalfd_ioctl,
};

static inline int _is_signalfd(vfs_node_t *node) {
    return node && node->ops == &signalfd_ops;
}

static signalfd_state_t *_state(vfs_node_t *node) {
    if (!_is_signalfd(node)) return 0;
    return (signalfd_state_t *)node->priv;
}

/* Signals that can never be caught this way. */
#define SFD_UNCATCHABLE (SIGKILL | SIGSTOP)

static uint32_t _pending_for(signalfd_state_t *s) {
    if (!current_task || !current_task->proc) return 0;
    uint32_t blocked = current_task->proc->signal_mask;
    uint32_t pending = current_task->proc->pending_signals;
    return pending & s->mask & blocked & ~SFD_UNCATCHABLE;
}

static int _signalfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    signalfd_state_t *s = _state(node);
    if (!s) return -1;
    if (!current_task || !current_task->proc) return -1;
    if (size < SIGNALFD_INFO_SIZE) return -EINVAL;

    for (;;) {
        mutex_lock(&s->lock);
        uint32_t c = _pending_for(s);
        if (c != 0) {
            int bit = 0;
            while (!(c & (1u << bit))) bit++;
            current_task->proc->pending_signals &= ~(1u << bit);
            mutex_unlock(&s->lock);

            uint8_t info[SIGNALFD_INFO_SIZE];
            memset(info, 0, sizeof(info));
            uint32_t signo = (uint32_t)bit;
            __builtin_memcpy(info, &signo, 4);
            __builtin_memcpy(buf, info, SIGNALFD_INFO_SIZE);
            return SIGNALFD_INFO_SIZE;
        }
        int nonblock = (s->flags & CACT_SFD_NONBLOCK);
        mutex_unlock(&s->lock);
        if (nonblock) return -EAGAIN;
        schedule();
        if (!_is_signalfd(node)) return -1;
    }
}

static int _signalfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node; (void)off; (void)size; (void)buf;
    return -EINVAL;
}

static void _signalfd_open(vfs_node_t *node) {
    if (_is_signalfd(node)) node->refcount++;
}

static void _signalfd_close(vfs_node_t *node) {
    if (!_is_signalfd(node)) return;
    if (node->refcount > 0) node->refcount--;
    if (node->refcount == 0) {
        if (node->priv) kfree(node->priv);
        node->priv = 0;
        kfree(node);
    }
}

static int _signalfd_poll(vfs_node_t *node, uint32_t events) {
    signalfd_state_t *s = _state(node);
    if (!s) return VFS_POLLERR;
    uint32_t revents = 0;
    mutex_lock(&s->lock);
    if (_pending_for(s) != 0) {
        if (events & VFS_POLLIN) revents |= VFS_POLLIN;
    }
    mutex_unlock(&s->lock);
    return (int)revents;
}

static int _signalfd_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    signalfd_state_t *s = _state(node);
    if (!s) return -ENOTTY;
    if (cmd != CACT_SIGNALFD_SETMASK) return -ENOTTY;
    if (!arg) return -EINVAL;
    cact_signalfd_create_arg_t a;
    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    mutex_lock(&s->lock);
    s->mask = a.mask;
    mutex_unlock(&s->lock);
    return 0;
}

vfs_node_t *signalfd_create_vnode(uint32_t mask, uint32_t flags) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    memset(node, 0, sizeof(vfs_node_t));

    signalfd_state_t *s = (signalfd_state_t *)kmalloc(sizeof(signalfd_state_t));
    if (!s) {
        kfree(node);
        return 0;
    }
    memset(s, 0, sizeof(signalfd_state_t));
    s->mask  = mask & ~SFD_UNCATCHABLE;
    s->flags = flags & CACT_SFD_NONBLOCK;
    mutex_init(&s->lock);

    strlcpy(node->name, "signalfd", 128);
    node->type     = VFS_FILE;
    node->inode    = 0;
    node->refcount = 0;
    node->ops      = &signalfd_ops;
    node->priv     = s;
    return node;
}
