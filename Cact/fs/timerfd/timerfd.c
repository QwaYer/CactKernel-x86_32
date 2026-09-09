#include "timerfd.h"
#include "task.h"
#include "sync.h"
#include "klib.h"
#include "helper.h"
#include "ioctl_abi.h"
#include "validate.h"

// timerfd.c — timerfd(2) VFS node.
//
// The kernel clock is a 100 Hz monotonic tick (10 ms).  Deadlines are stored
// as absolute ticks; expiry is evaluated lazily in read()/poll()/ioctl()
// against timer_ticks_get(), matching the cooperative poll-and-reschedule
// model used everywhere else (no per-tick node list).
//
// read(2) returns a uint64 count of expirations that happened since the last
// read.  write(2) is not supported (Linux rejects it too).

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

#define TIMERFD_HZ 100

typedef struct timerfd_state {
    uint32_t expiry_at;       /* absolute tick deadline; 0 = disarmed */
    uint32_t interval_ticks;  /* periodic re-arm; 0 = one-shot         */
    uint64_t count;           /* pending expirations                  */
    uint32_t flags;           /* CACT_TFD_NONBLOCK                    */
    mutex_t  lock;
} timerfd_state_t;

static int _timerfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _timerfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _timerfd_open(vfs_node_t *node);
static void _timerfd_close(vfs_node_t *node);
static int _timerfd_poll(vfs_node_t *node, uint32_t events);
static int _timerfd_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);

static vfs_ops_t timerfd_ops = {
    .read  = _timerfd_read,
    .write = _timerfd_write,
    .open  = _timerfd_open,
    .close = _timerfd_close,
    .poll  = _timerfd_poll,
    .ioctl = _timerfd_ioctl,
};

static inline int _is_timerfd(vfs_node_t *node) {
    return node && node->ops == &timerfd_ops;
}

static timerfd_state_t *_state(vfs_node_t *node) {
    if (!_is_timerfd(node)) return 0;
    return (timerfd_state_t *)node->priv;
}

static inline int _expired(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

/* Count every expiry whose deadline has passed and re-arm.  Caller holds the
 * lock and passes a stable `now`. */
static void _tfd_sweep(timerfd_state_t *s, uint32_t now) {
    uint32_t guard = 0;
    while (s->expiry_at != 0 && _expired(now, s->expiry_at)) {
        s->count++;
        if (s->interval_ticks == 0) {
            s->expiry_at = 0;
            break;
        }
        uint32_t next = s->expiry_at + s->interval_ticks;
        /* expiry_at advances monotonically; a wrap means the wheel is done */
        if (next <= s->expiry_at || ++guard > 100000) {
            s->expiry_at = 0;
            break;
        }
        s->expiry_at = next;
    }
}

static int _timerfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    timerfd_state_t *s = _state(node);
    if (!s) return -1;
    if (size < 8) return -EINVAL;

    for (;;) {
        mutex_lock(&s->lock);
        _tfd_sweep(s, timer_ticks_get());
        if (s->count > 0) {
            uint64_t v = s->count;
            s->count = 0;
            mutex_unlock(&s->lock);
            __builtin_memcpy(buf, &v, 8);
            return 8;
        }
        if (s->flags & CACT_TFD_NONBLOCK) {
            mutex_unlock(&s->lock);
            return -EAGAIN;
        }
        mutex_unlock(&s->lock);
        schedule();
        if (!_is_timerfd(node)) return -1;
    }
}

static int _timerfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node; (void)off; (void)size; (void)buf;
    return -EINVAL;
}

static void _timerfd_open(vfs_node_t *node) {
    if (_is_timerfd(node)) node->refcount++;
}

static void _timerfd_close(vfs_node_t *node) {
    if (!_is_timerfd(node)) return;
    if (node->refcount > 0) node->refcount--;
    if (node->refcount == 0) {
        if (node->priv) kfree(node->priv);
        node->priv = 0;
        kfree(node);
    }
}

static int _timerfd_poll(vfs_node_t *node, uint32_t events) {
    timerfd_state_t *s = _state(node);
    if (!s) return VFS_POLLERR;
    uint32_t revents = 0;
    mutex_lock(&s->lock);
    _tfd_sweep(s, timer_ticks_get());
    if (s->count > 0 && (events & VFS_POLLIN)) revents |= VFS_POLLIN;
    mutex_unlock(&s->lock);
    return (int)revents;
}

static void _spec_to_state(timerfd_state_t *s, const cact_timerfd_spec_t *a,
                           uint32_t now) {
    uint32_t value_ticks = 0;
    uint32_t interval_ticks = 0;
    if (a->it_value_ms != 0) {
        value_ticks = (a->it_value_ms + (1000 / TIMERFD_HZ) - 1) / (1000 / TIMERFD_HZ);
        if (value_ticks == 0) value_ticks = 1;
    }
    if (a->it_interval_ms != 0) {
        interval_ticks = (a->it_interval_ms + (1000 / TIMERFD_HZ) - 1) / (1000 / TIMERFD_HZ);
        if (interval_ticks == 0) interval_ticks = 1;
    }
    s->interval_ticks = interval_ticks;
    s->count = 0;
    if (value_ticks == 0) {
        s->expiry_at = 0;
        return;
    }
    if (a->flags & CACT_TFD_TIMER_ABSTIME) {
        uint32_t target = value_ticks;
        if (_expired(now, target)) {
            /* absolute deadline already passed: expire immediately */
            s->expiry_at = now - 1;
        } else {
            s->expiry_at = target;
        }
    } else {
        s->expiry_at = now + value_ticks;
    }
}

static void _state_to_spec(timerfd_state_t *s, uint32_t now, uint32_t old_flags,
                           cact_timerfd_spec_t *out) {
    (void)old_flags;
    out->it_interval_ms = s->interval_ticks * (1000 / TIMERFD_HZ);
    if (s->expiry_at == 0) {
        out->it_value_ms = 0;
    } else if (_expired(now, s->expiry_at)) {
        out->it_value_ms = 0;
    } else {
        uint32_t left = s->expiry_at - now;
        out->it_value_ms = left * (1000 / TIMERFD_HZ);
        if (out->it_value_ms == 0) out->it_value_ms = 1;
    }
}

static int _timerfd_settime(timerfd_state_t *s, const cact_timerfd_spec_t *a,
                            cact_timerfd_spec_t *out) {
    mutex_lock(&s->lock);
    uint32_t now = timer_ticks_get();
    uint32_t old_value_ms = 0;
    if (s->expiry_at != 0 && !_expired(now, s->expiry_at)) {
        old_value_ms = (s->expiry_at - now) * (1000 / TIMERFD_HZ);
        if (old_value_ms == 0) old_value_ms = 1;
    }
    out->old_value_ms    = old_value_ms;
    out->old_interval_ms = s->interval_ticks * (1000 / TIMERFD_HZ);
    _spec_to_state(s, a, now);
    mutex_unlock(&s->lock);
    return 0;
}

static int _timerfd_gettime(timerfd_state_t *s, cact_timerfd_spec_t *out) {
    mutex_lock(&s->lock);
    _tfd_sweep(s, timer_ticks_get());
    out->flags = 0;
    _state_to_spec(s, timer_ticks_get(), 0, out);
    mutex_unlock(&s->lock);
    return 0;
}

static int _timerfd_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    timerfd_state_t *s = _state(node);
    if (!s) return -ENOTTY;
    switch (cmd) {
    case CACT_TIMERFD_SETTIME: {
        if (!arg) return -EINVAL;
        cact_timerfd_spec_t a;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        cact_timerfd_spec_t out;
        out.flags = 0;
        int r = _timerfd_settime(s, &a, &out);
        if (r < 0) return r;
        return copy_to_user(arg, &out, sizeof(out));
    }
    case CACT_TIMERFD_GETTIME: {
        if (!arg) return -EINVAL;
        cact_timerfd_spec_t out;
        out.flags = 0;
        int r = _timerfd_gettime(s, &out);
        if (r < 0) return r;
        return copy_to_user(arg, &out, sizeof(out));
    }
    default:
        return -ENOTTY;
    }
}

vfs_node_t *timerfd_create_vnode(int32_t clockid, uint32_t flags) {
    (void)clockid;   /* single monotonic clock */

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    memset(node, 0, sizeof(vfs_node_t));

    timerfd_state_t *s = (timerfd_state_t *)kmalloc(sizeof(timerfd_state_t));
    if (!s) {
        kfree(node);
        return 0;
    }
    memset(s, 0, sizeof(timerfd_state_t));
    s->flags = flags & CACT_TFD_NONBLOCK;
    mutex_init(&s->lock);

    strlcpy(node->name, "timerfd", 128);
    node->type     = VFS_FILE;
    node->inode    = 0;
    node->refcount = 0;
    node->ops      = &timerfd_ops;
    node->priv     = s;
    return node;
}
