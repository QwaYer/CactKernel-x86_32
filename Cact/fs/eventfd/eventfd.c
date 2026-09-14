#include "eventfd.h"
#include "task.h"
#include "sync.h"
#include "klib.h"
#include "kernel.h"
#include "helper.h"
#include "ioctl_abi.h"

// eventfd.c — eventfd(2) VFS node.
//
// A 64-bit counter.  read(2): if EFD_SEMAPHORE, return 1 and decrement;
// otherwise return the counter and reset it to 0.  Blocks (schedule loop)
// while the counter is 0 unless EFD_NONBLOCK.  write(2) adds an 8-byte value.
// poll(2): readable while the counter is non-zero.
//
// Lifetime mirrors memfd.c: the vnode is created with refcount 0, bumped by
// ops->open (file_alloc/fork) and dropped by ops->close, which frees the
// node+state at zero.

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

#define EVENTFD_MAX_COUNTER (~0ull - 1ull)   /* all-ones write = invalid */

typedef struct eventfd_state {
    uint64_t counter;
    uint32_t flags;      /* CACT_EFD_SEMAPHORE | CACT_EFD_NONBLOCK */
    mutex_t  lock;
} eventfd_state_t;

static int _eventfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _eventfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _eventfd_open(vfs_node_t *node);
static void _eventfd_close(vfs_node_t *node);
static int _eventfd_poll(vfs_node_t *node, uint32_t events);

static vfs_ops_t eventfd_ops = {
    .read  = _eventfd_read,
    .write = _eventfd_write,
    .open  = _eventfd_open,
    .close = _eventfd_close,
    .poll  = _eventfd_poll,
};

static inline int _is_eventfd(vfs_node_t *node) {
    return node && node->ops == &eventfd_ops;
}

static eventfd_state_t *_state(vfs_node_t *node) {
    if (!_is_eventfd(node)) return 0;
    return (eventfd_state_t *)node->priv;
}

static int _eventfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    eventfd_state_t *s = _state(node);
    if (!s) return -1;
    if (size < 8) return -EINVAL;

    for (;;) {
        mutex_lock(&s->lock);
        int readable = (s->counter > 0);
        if (!readable && (s->flags & CACT_EFD_NONBLOCK)) {
            mutex_unlock(&s->lock);
            return -EAGAIN;
        }
        if (readable) {
            uint64_t v;
            if (s->flags & CACT_EFD_SEMAPHORE) {
                v = 1;
                s->counter--;
            } else {
                v = s->counter;
                s->counter = 0;
            }
            mutex_unlock(&s->lock);
            __builtin_memcpy(buf, &v, 8);
            return 8;
        }
        mutex_unlock(&s->lock);
        schedule();
        if (!_is_eventfd(node)) return -1;
    }
}

static int _eventfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    eventfd_state_t *s = _state(node);
    if (!s) return -1;
    if (size < 8) return -EINVAL;

    uint64_t add;
    __builtin_memcpy(&add, buf, 8);
    if (add == ~0ull) return -EINVAL;

    for (;;) {
        mutex_lock(&s->lock);
        int fits = (s->counter <= EVENTFD_MAX_COUNTER - add);
        if (!fits && (s->flags & CACT_EFD_NONBLOCK)) {
            mutex_unlock(&s->lock);
            return -EAGAIN;
        }
        if (fits) {
            s->counter += add;
            mutex_unlock(&s->lock);
            return 8;
        }
        mutex_unlock(&s->lock);
        schedule();
        if (!_is_eventfd(node)) return -1;
    }
}

static void _eventfd_open(vfs_node_t *node) {
    if (_is_eventfd(node)) node->refcount++;
}

static void _eventfd_close(vfs_node_t *node) {
    if (!_is_eventfd(node)) return;
    if (node->refcount > 0) node->refcount--;
    if (node->refcount == 0) {
        if (node->priv) kfree(node->priv);
        node->priv = 0;
        kfree(node);
    }
}

static int _eventfd_poll(vfs_node_t *node, uint32_t events) {
    eventfd_state_t *s = _state(node);
    if (!s) return VFS_POLLERR;
    uint32_t revents = 0;
    mutex_lock(&s->lock);
    if (s->counter > 0) {
        if (events & VFS_POLLIN) revents |= VFS_POLLIN;
    }
    mutex_unlock(&s->lock);
    return (int)revents;
}

vfs_node_t *eventfd_create_vnode(uint32_t initval, uint32_t flags) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        pr_err("  %-11s : cannot allocate node\n", "eventfd");
        return 0;
    }
    memset(node, 0, sizeof(vfs_node_t));

    eventfd_state_t *s = (eventfd_state_t *)kmalloc(sizeof(eventfd_state_t));
    if (!s) {
        kfree(node);
        pr_err("  %-11s : cannot allocate state\n", "eventfd");
        return 0;
    }
    memset(s, 0, sizeof(eventfd_state_t));
    s->counter = (uint64_t)initval;
    s->flags   = flags & (CACT_EFD_SEMAPHORE | CACT_EFD_NONBLOCK);
    mutex_init(&s->lock);

    strlcpy(node->name, "eventfd", 128);
    node->type     = VFS_FILE;
    node->inode    = 0;
    node->refcount = 0;
    node->ops      = &eventfd_ops;
    node->priv     = s;
    return node;
}
