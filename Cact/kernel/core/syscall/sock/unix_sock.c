#include "unix_sock.h"
#include "socket.h"
#include "ioctl_abi.h"
#include "kernel.h"
#include "task.h"
#include "klib.h"
#include "sync.h"
#include "memory.h"
#include "validate.h"
#include "helper.h"

// unix_sock.c — AF_UNIX (local) stream sockets.
//
// Model (deliberately close to pipe.c):
//   * every open endpoint is a `unix_ep_t` owning its receive ring buffer;
//   * a connected pair A<->B is two endpoints.  ep_link() makes each side
//     hold a reference on the peer, so a peer can never vanish while a write
//     syscall on the other side is still in flight;
//   * ep_break() tears a connection down from one side: it clears both peer
//     pointers and drops BOTH peer-holds (the one we hold on the peer and the
//     one the peer holds on us);
//   * a listening endpoint owns a FIFO of accepted-but-not-yet-accept()ed
//     server endpoints (each held by the queue);
//   * blocking reads/writes/accepts use the kernel's cooperative
//     schedule() loop exactly like pipe.c;
//   * bound names live in a small registry keyed by absolute pathname.
//     No filesystem inode is created for the socket path itself yet.

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EMFILE
#define EMFILE 24
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EPIPE
#define EPIPE 32
#endif
#ifndef EADDRINUSE
#define EADDRINUSE 98
#endif
#ifndef ENOTSOCK
#define ENOTSOCK 88
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif

#define UNIX_MAGIC      0x554E4958u   /* 'UNIX' */
#define UNIX_PATH_MAX   108
#define UNIX_BUF_SIZE   4096
#define UNIX_BACKLOG    4
#define UNIX_BOUND_MAX  32

typedef enum {
    UEP_NONE = 0,
    UEP_BOUND,
    UEP_LISTEN,
    UEP_CONN,
} uep_kind_t;

typedef struct unix_ep {
    uint32_t magic;
    uint8_t  kind;          /* UEP_* */
    uint8_t  used;
    uint8_t  shut_rd;       /* local SHUT_RD applied: reads stop */
    uint8_t  shut_wr;       /* local SHUT_WR applied: writes fail */
    uint8_t  eof;           /* peer closed / did SHUT_WR: reads drain then EOF */
    uint8_t  peer_no_read;  /* peer shut read side / is gone: writes fail */
    struct unix_ep *peer;   /* other end of the connection (NULL when gone) */
    uint32_t refs;          /* node wrappers + queue holds + peer holds */

    mutex_t  lock;

    /* incoming bytes (written by the peer) — ring buffer */
    uint8_t  rx[UNIX_BUF_SIZE];
    uint32_t rx_len;
    uint32_t rx_off;

    /* listening endpoint state */
    char     bound[UNIX_PATH_MAX];
    int      backlog;
    struct unix_ep *pend_head;
    struct unix_ep *pend_tail;
    struct unix_ep *pend_next;
    uint32_t pend_count;

    vfs_node_t *node;
} unix_ep_t;

/* bound-name registry (heap-allocated to keep the kernel .bss tail small) */
typedef struct {
    uint8_t used;
    char    path[UNIX_PATH_MAX];
    unix_ep_t *ep;
} unix_bound_t;

static unix_bound_t *unix_bound;
static mutex_t *unix_bound_lock;
static int unix_ready;

static vfs_ops_t unix_sock_ops;

/* ── forward declarations ─────────────────────────────────────────────── */

static unix_ep_t *ep_from_node(vfs_node_t *node);
static vfs_node_t *unix_make_node(unix_ep_t *ep);
static void ep_put(unix_ep_t *ep);
static void ep_destroy(unix_ep_t *ep);
static int unix_path_canon(const char *path, char *out, int out_max);
static int unix_bound_insert(const char *path, unix_ep_t *ep);
static void unix_bound_remove(unix_ep_t *ep);

/* ── small helpers ────────────────────────────────────────────────────── */

static unix_ep_t *ep_alloc(void) {
    unix_ep_t *ep = (unix_ep_t *)kmalloc(sizeof(unix_ep_t));
    if (!ep) return 0;
    memset(ep, 0, sizeof(unix_ep_t));
    ep->magic   = UNIX_MAGIC;
    ep->used    = 1;
    ep->kind    = UEP_NONE;
    ep->backlog = UNIX_BACKLOG;
    mutex_init(&ep->lock);
    return ep;
}

/* link two endpoints into a connection: each holds a reference on the other */
static void ep_link(unix_ep_t *a, unix_ep_t *b) {
    mutex_lock(&a->lock);
    a->peer = b;
    a->refs++;
    mutex_unlock(&a->lock);

    mutex_lock(&b->lock);
    b->peer = a;
    b->refs++;
    mutex_unlock(&b->lock);
}

/* break a connection from ep's side; caller must not hold ep->lock */
static void ep_break(unix_ep_t *ep) {
    unix_ep_t *peer;
    int dead_self = 0, dead_peer = 0;

    mutex_lock(&ep->lock);
    peer = ep->peer;
    if (peer) {
        ep->peer = NULL;
        /* release the peer-hold the peer kept on us */
        if (ep->refs > 0) ep->refs--;
    }
    dead_self = (ep->refs == 0);
    mutex_unlock(&ep->lock);

    if (peer) {
        mutex_lock(&peer->lock);
        peer->peer = NULL;
        peer->eof = 1;
        peer->peer_no_read = 1;
        if (peer->refs > 0) peer->refs--;   /* release our hold on the peer */
        dead_peer = (peer->refs == 0);
        mutex_unlock(&peer->lock);
    }

    if (dead_self) ep_destroy(ep);
    if (peer && dead_peer) ep_destroy(peer);
}

/* destroy an endpoint that has reached refs == 0 (caller holds no locks) */
static void ep_destroy(unix_ep_t *ep) {
    if (!ep) return;

    mutex_lock(&ep->lock);
    if (!ep->used) { mutex_unlock(&ep->lock); return; }
    ep->used = 0;

    unix_ep_t *peer = ep->peer;      /* normally NULL after ep_break */
    ep->peer = NULL;
    unix_ep_t *pend = ep->pend_head;
    ep->pend_head = NULL;
    ep->pend_tail = NULL;
    ep->pend_count = 0;
    int was_bound = (ep->kind == UEP_BOUND || ep->kind == UEP_LISTEN);
    mutex_unlock(&ep->lock);

    if (was_bound)
        unix_bound_remove(ep);

    if (peer) {
        mutex_lock(&peer->lock);
        peer->peer = NULL;
        peer->eof = 1;
        peer->peer_no_read = 1;
        if (peer->refs > 0) peer->refs--;
        int dead_peer = (peer->refs == 0);
        mutex_unlock(&peer->lock);
        if (dead_peer) ep_destroy(peer);
    }

    while (pend) {          /* drop queue holds on pending server endpoints */
        unix_ep_t *next = pend->pend_next;
        pend->pend_next = NULL;
        ep_put(pend);
        pend = next;
    }

    ep->magic = 0;
    kfree(ep);
}

static void ep_put(unix_ep_t *ep) {
    if (!ep) return;
    mutex_lock(&ep->lock);
    if (ep->refs > 0) ep->refs--;
    uint32_t dead = (ep->refs == 0);
    mutex_unlock(&ep->lock);
    if (dead) ep_destroy(ep);
}

/* close a connected endpoint: break the link, drop the node holder */
static void unix_node_close_conn(unix_ep_t *ep) {
    mutex_lock(&ep->lock);
    int connected = (ep->peer != NULL);
    mutex_unlock(&ep->lock);
    if (connected) ep_break(ep);
    ep_put(ep);
}

/* ── path registry ────────────────────────────────────────────────────── */

void unix_sock_init(void) {
    if (unix_ready) return;
    if (!unix_bound_lock)
        unix_bound_lock = (mutex_t *)kmalloc(sizeof(mutex_t));
    if (!unix_bound)
        unix_bound = (unix_bound_t *)kmalloc(sizeof(unix_bound_t) * UNIX_BOUND_MAX);
    if (!unix_bound_lock || !unix_bound) {
        printk("unix_sock: registry alloc failed\n");
        return;
    }
    mutex_init(unix_bound_lock);
    memset(unix_bound, 0, sizeof(unix_bound_t) * UNIX_BOUND_MAX);
    unix_ready = 1;
}

/* make an absolute, collapsed path for registry keying */
static int unix_path_canon(const char *path, char *out, int out_max) {
    if (!path) return -EINVAL;
    if (path[0] == '\0') {
        if (path[1] != '\0') return -EOPNOTSUPP;  /* abstract namespace */
        return -EINVAL;                            /* empty path */
    }
    if (current_task) {
        vfs_make_abs(path, out, out_max);
    } else {
        int i = 0;
        while (path[i] && i < out_max - 1) { out[i] = path[i]; i++; }
        out[i] = '\0';
    }
    return 0;
}

static int unix_bound_insert(const char *path, unix_ep_t *ep) {
    int r = -EADDRINUSE;
    mutex_lock(unix_bound_lock);
    for (int i = 0; i < UNIX_BOUND_MAX; i++) {
        if (unix_bound[i].used && streq(unix_bound[i].path, path)) {
            mutex_unlock(unix_bound_lock);
            return -EADDRINUSE;
        }
    }
    for (int i = 0; i < UNIX_BOUND_MAX; i++) {
        if (!unix_bound[i].used) {
            int j = 0;
            while (path[j] && j < UNIX_PATH_MAX - 1) {
                unix_bound[i].path[j] = path[j];
                j++;
            }
            unix_bound[i].path[j] = '\0';
            unix_bound[i].ep  = ep;
            unix_bound[i].used = 1;
            r = 0;
            break;
        }
    }
    mutex_unlock(unix_bound_lock);
    return r;
}

static void unix_bound_remove(unix_ep_t *ep) {
    mutex_lock(unix_bound_lock);
    for (int i = 0; i < UNIX_BOUND_MAX; i++) {
        if (unix_bound[i].used && unix_bound[i].ep == ep) {
            unix_bound[i].used = 0;
            unix_bound[i].ep = 0;
            unix_bound[i].path[0] = '\0';
            break;
        }
    }
    mutex_unlock(unix_bound_lock);
}

/* find a bound listener by canonical path; returns with an extra reference
   that the caller must release with ep_put() */
static unix_ep_t *unix_bound_lookup(const char *path) {
    unix_ep_t *found = 0;
    mutex_lock(unix_bound_lock);
    for (int i = 0; i < UNIX_BOUND_MAX; i++) {
        if (unix_bound[i].used && streq(unix_bound[i].path, path)) {
            found = unix_bound[i].ep;
            if (found) {
                mutex_lock(&found->lock);
                if (found->used) found->refs++;
                else found = 0;
                mutex_unlock(&found->lock);
            }
            break;
        }
    }
    mutex_unlock(unix_bound_lock);
    return found;
}

/* ── VFS node wrapping ────────────────────────────────────────────────── */

static vfs_node_t *unix_make_node(unix_ep_t *ep) {
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return 0;
    memset(n, 0, sizeof(vfs_node_t));
    n->type     = VFS_SOCKET;
    n->refcount = 1;
    n->ops      = &unix_sock_ops;
    n->priv     = ep;

    mutex_lock(&ep->lock);
    ep->node = n;
    ep->refs++;
    mutex_unlock(&ep->lock);
    return n;
}

static unix_ep_t *ep_from_node(vfs_node_t *node) {
    if (!node || node->type != VFS_SOCKET || node->ops != &unix_sock_ops)
        return 0;
    unix_ep_t *ep = (unix_ep_t *)node->priv;
    if (!ep || ep->magic != UNIX_MAGIC || !ep->used) return 0;
    return ep;
}

int unix_sock_is_node(vfs_node_t *node) {
    if (!node || node->type != VFS_SOCKET) return 0;
    return node->ops == &unix_sock_ops;
}

/* ── ring-buffer push/pop ─────────────────────────────────────────────── */

static void ep_push(unix_ep_t *ep, const char *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pos = (ep->rx_off + ep->rx_len) % UNIX_BUF_SIZE;
        ep->rx[pos] = (uint8_t)src[i];
        ep->rx_len++;
    }
}

static uint32_t ep_pop(unix_ep_t *ep, char *dst, uint32_t max) {
    uint32_t n = (ep->rx_len < max) ? ep->rx_len : max;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = (char)ep->rx[ep->rx_off];
        ep->rx_off = (ep->rx_off + 1) % UNIX_BUF_SIZE;
    }
    ep->rx_len -= n;
    if (ep->rx_len == 0) ep->rx_off = 0;
    return n;
}

/* ── VFS ops ──────────────────────────────────────────────────────────── */

static void unix_node_open(vfs_node_t *node) {
    (void)node;
}

/* socket fd closed for good */
static void unix_node_close(vfs_node_t *node) {
    unix_ep_t *ep = (unix_ep_t *)node->priv;
    node->priv = NULL;
    if (ep && ep->magic == UNIX_MAGIC && ep->used) {
        if (ep->kind == UEP_CONN) {
            unix_node_close_conn(ep);
        } else {
            ep_destroy(ep);   /* bound/listen: unbind, drop pending queue */
        }
    }
    kfree(node);
}

static int unix_node_read(vfs_node_t *node, uint32_t off, uint32_t size,
                          char *buf) {
    (void)off;
    unix_ep_t *ep = ep_from_node(node);
    if (!ep || size == 0) return -1;
    if (ep->kind != UEP_CONN) return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    for (;;) {
        mutex_lock(&ep->lock);
        if (ep->shut_rd) { mutex_unlock(&ep->lock); return 0; }
        if (ep->rx_len > 0) {
            uint32_t n = ep_pop(ep, buf, size);
            mutex_unlock(&ep->lock);
            return (n > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)n;
        }
        if (ep->eof) {          /* peer closed / SHUT_WR: EOF */
            mutex_unlock(&ep->lock);
            return 0;
        }
        mutex_unlock(&ep->lock);
        if (!validate_user_ptr(buf, size)) return -1;
        schedule();
    }
}

static int unix_node_write(vfs_node_t *node, uint32_t off, uint32_t size,
                           char *buf) {
    (void)off;
    unix_ep_t *ep = ep_from_node(node);
    if (!ep || size == 0) return -1;
    if (ep->kind != UEP_CONN) return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    uint32_t written = 0;
    for (;;) {
        if (ep->shut_wr) return written ? (int)written : -EPIPE;
        unix_ep_t *peer = ep->peer;   /* stable while this endpoint is open */
        if (!peer) {
            if (current_task) task_signal(current_task->pid, SIGPIPE);
            return written ? (int)written : -EPIPE;
        }

        mutex_lock(&peer->lock);
        if (!peer->used || peer->peer_no_read || peer->shut_rd) {
            mutex_unlock(&peer->lock);
            if (current_task) task_signal(current_task->pid, SIGPIPE);
            return written ? (int)written : -EPIPE;
        }
        uint32_t space = UNIX_BUF_SIZE - peer->rx_len;
        if (space == 0) {
            mutex_unlock(&peer->lock);
            if (!validate_user_ptr(buf + written, 1))
                return written ? (int)written : -1;
            schedule();
            continue;
        }
        uint32_t chunk = size - written;
        if (chunk > space) chunk = space;
        ep_push(peer, buf + written, chunk);
        written += chunk;
        mutex_unlock(&peer->lock);
        if (written == size) break;
        if (!validate_user_ptr(buf + written, 1))
            return written ? (int)written : -1;
        schedule();
    }
    return (written > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)written;
}

static int unix_node_poll(vfs_node_t *node, uint32_t events) {
    unix_ep_t *ep = ep_from_node(node);
    if (!ep) return VFS_POLLERR;

    uint32_t revents = 0;
    mutex_lock(&ep->lock);

    if (ep->kind == UEP_LISTEN) {
        if ((events & VFS_POLLIN) && ep->pend_count > 0) revents |= VFS_POLLIN;
        if (events & VFS_POLLOUT) revents |= VFS_POLLOUT;
    } else if (ep->kind == UEP_BOUND) {
        if (events & VFS_POLLOUT) revents |= VFS_POLLOUT;
    } else {
        if ((events & VFS_POLLIN) && (ep->rx_len > 0 || ep->eof))
            revents |= VFS_POLLIN;
        if ((events & VFS_POLLOUT) && !ep->shut_wr &&
            !ep->peer_no_read && ep->peer)
            revents |= VFS_POLLOUT;
        if (ep->peer_no_read || !ep->peer) revents |= VFS_POLLERR;
        if (ep->eof && ep->rx_len == 0)    revents |= VFS_POLLHUP;
    }
    mutex_unlock(&ep->lock);
    return (int)revents;
}

static vfs_ops_t unix_sock_ops = {
    .read   = unix_node_read,
    .write  = unix_node_write,
    .open   = unix_node_open,
    .close  = unix_node_close,
    .poll   = unix_node_poll,
    .lseek  = 0,
};

/* ── public creation ──────────────────────────────────────────────────── */

vfs_node_t *unix_sock_create(int type, int protocol) {
    if (!unix_ready) unix_sock_init();
    if (type != SOCK_STREAM) return 0;
    (void)protocol;

    unix_ep_t *ep = ep_alloc();
    if (!ep) return 0;

    vfs_node_t *node = unix_make_node(ep);
    if (!node) {
        ep->used = 0;
        kfree(ep);
        return 0;
    }
    return node;
}

int unix_socketpair(int type, int fds[2]) {
    if (type != SOCK_STREAM) return -EOPNOTSUPP;
    if (!current_task) return -1;
    if (!unix_ready) unix_sock_init();

    unix_ep_t *a = ep_alloc();
    unix_ep_t *b = ep_alloc();
    if (!a || !b) {
        if (a) { a->used = 0; kfree(a); }
        if (b) { b->used = 0; kfree(b); }
        return -ENOMEM;
    }
    a->kind = UEP_CONN;
    b->kind = UEP_CONN;
    ep_link(a, b);

    vfs_node_t *na = unix_make_node(a);
    vfs_node_t *nb = 0;
    if (na) nb = unix_make_node(b);

    if (!na || !nb) {
        /* tear the link down both ways, then drop whatever node holders
           were already created (ep_break() frees both endpoints when the
           failing side had no node holder yet) */
        ep_break(a);
        if (na) { na->priv = NULL; ep_put(a); kfree(na); }
        if (nb) { nb->priv = NULL; ep_put(b); kfree(nb); }
        return -ENOMEM;
    }

    int fa = alloc_fd(na);
    if (fa < 0) {
        unix_node_close(na);
        unix_node_close(nb);
        return -EMFILE;
    }
    int fb = alloc_fd(nb);
    if (fb < 0) {
        unix_node_close(nb);
        file_t *f = current_task->proc->fds->files[fa];
        current_task->proc->fds->files[fa] = 0;
        if (f) file_unref(f);
        return -EMFILE;
    }
    fds[0] = fa;
    fds[1] = fb;
    return 0;
}

/* ── SOCKCTL_* ioctl dispatcher ───────────────────────────────────────── */

static int unix_bind_ioctl(unix_ep_t *ep, void *arg) {
    mutex_lock(&ep->lock);
    int busy = (ep->kind != UEP_NONE);
    mutex_unlock(&ep->lock);
    if (busy) return -EINVAL;
    if (!arg) return -EINVAL;

    cact_unix_addr_t ua;
    if (copy_from_user(&ua, arg, sizeof(ua)) != 0) return -EFAULT;
    ua.path[sizeof(ua.path) - 1] = '\0';

    char canon[512];
    int r = unix_path_canon(ua.path, canon, sizeof(canon));
    if (r < 0) return r;
    if (canon[0] != '/') return -EINVAL;

    r = unix_bound_insert(canon, ep);
    if (r < 0) return r;

    mutex_lock(&ep->lock);
    ep->kind = UEP_BOUND;
    int i = 0;
    while (canon[i] && i < UNIX_PATH_MAX - 1) { ep->bound[i] = canon[i]; i++; }
    ep->bound[i] = '\0';
    mutex_unlock(&ep->lock);
    return 0;
}

static int unix_connect_ioctl(unix_ep_t *ep, void *arg) {
    mutex_lock(&ep->lock);
    int busy = (ep->kind != UEP_NONE);
    mutex_unlock(&ep->lock);
    if (busy) return -EINVAL;
    if (!arg) return -EINVAL;

    cact_unix_addr_t ua;
    if (copy_from_user(&ua, arg, sizeof(ua)) != 0) return -EFAULT;
    ua.path[sizeof(ua.path) - 1] = '\0';

    char canon[512];
    int r = unix_path_canon(ua.path, canon, sizeof(canon));
    if (r < 0) return r;

    unix_ep_t *lis = unix_bound_lookup(canon);
    if (!lis) return -ENOENT;

    mutex_lock(&lis->lock);
    int ok = (lis->kind == UEP_LISTEN &&
              lis->pend_count < (uint32_t)lis->backlog);
    mutex_unlock(&lis->lock);
    if (!ok) {
        ep_put(lis);
        return -ECONNREFUSED;
    }

    /* server-side endpoint that accept() will later return */
    unix_ep_t *srv = ep_alloc();
    if (!srv) {
        ep_put(lis);
        return -ENOMEM;
    }
    srv->kind = UEP_CONN;

    ep_link(srv, ep);          /* srv <-> client, peer holds both ways */

    mutex_lock(&ep->lock);
    ep->kind = UEP_CONN;
    mutex_unlock(&ep->lock);

    /* queue the server endpoint on the listener */
    mutex_lock(&lis->lock);
    srv->pend_next = NULL;
    if (lis->pend_tail) lis->pend_tail->pend_next = srv;
    else                lis->pend_head = srv;
    lis->pend_tail = srv;
    lis->pend_count++;
    mutex_lock(&srv->lock);
    srv->refs++;               /* queue hold */
    mutex_unlock(&srv->lock);
    mutex_unlock(&lis->lock);

    ep_put(lis);               /* drop the lookup reference */
    return 0;
}

static int unix_listen_ioctl(unix_ep_t *ep, void *arg) {
    (void)arg;
    mutex_lock(&ep->lock);
    if (ep->kind != UEP_BOUND) { mutex_unlock(&ep->lock); return -EINVAL; }
    ep->kind = UEP_LISTEN;
    ep->backlog = UNIX_BACKLOG;
    mutex_unlock(&ep->lock);
    return 0;
}

static int unix_accept_ioctl(unix_ep_t *ep, void *arg) {
    (void)arg;
    mutex_lock(&ep->lock);
    int is_listen = (ep->kind == UEP_LISTEN);
    mutex_unlock(&ep->lock);
    if (!is_listen) return -EINVAL;

    for (;;) {
        unix_ep_t *srv = 0;
        mutex_lock(&ep->lock);
        if (ep->pend_count > 0) {
            srv = ep->pend_head;
            ep->pend_head = srv->pend_next;
            if (!ep->pend_head) ep->pend_tail = NULL;
            ep->pend_count--;
            srv->pend_next = NULL;
        }
        mutex_unlock(&ep->lock);

        if (srv) {
            vfs_node_t *node = unix_make_node(srv);
            if (!node) {
                ep_put(srv);       /* drop the queue hold */
                return -ENOMEM;
            }
            int fd = alloc_fd(node);
            if (fd < 0) {
                /* drop both the queue hold and the fresh node holder */
                node->priv = NULL;
                ep_put(srv);       /* node holder */
                ep_put(srv);       /* queue hold */
                kfree(node);
                return -EMFILE;
            }
            if (arg) {
                /* AF_UNIX peers are unnamed: report addrlen == 0 */
                cact_accept_arg_t z;
                memset(&z, 0, sizeof(z));
                if (copy_to_user(arg, &z, sizeof(z)) != 0) {
                    file_t *ff = current_task->proc->fds->files[fd];
                    current_task->proc->fds->files[fd] = 0;
                    if (ff) file_unref(ff);
                    ep_put(srv);   /* queue hold */
                    return -EFAULT;
                }
            }
            ep_put(srv);           /* drop the queue hold */
            return fd;
        }

        schedule();                /* no pending connection yet */
    }
}

static int unix_shutdown_ioctl(unix_ep_t *ep, void *arg) {
    if (!arg) return -EINVAL;
    uint32_t how;
    if (copy_from_user(&how, arg, sizeof(how)) != 0) return -EFAULT;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;

    if (how == SHUT_RD || how == SHUT_RDWR) {
        mutex_lock(&ep->lock);
        ep->shut_rd = 1;
        unix_ep_t *peer = ep->peer;
        mutex_unlock(&ep->lock);
        if (peer) {
            mutex_lock(&peer->lock);
            peer->peer_no_read = 1;   /* peer writes must fail now */
            mutex_unlock(&peer->lock);
        }
    }

    if (how == SHUT_WR || how == SHUT_RDWR) {
        mutex_lock(&ep->lock);
        ep->shut_wr = 1;
        unix_ep_t *peer = ep->peer;
        mutex_unlock(&ep->lock);
        if (peer) {
            mutex_lock(&peer->lock);
            peer->eof = 1;            /* peer reads drain then hit EOF */
            mutex_unlock(&peer->lock);
        }
    }
    return 0;
}

int unix_sock_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    unix_ep_t *ep = ep_from_node(node);
    if (!ep) return -ENOTSOCK;

    switch (cmd) {
    case CACT_SOCKCTL_UNIX_BIND:
        return unix_bind_ioctl(ep, arg);
    case CACT_SOCKCTL_UNIX_CONNECT:
        return unix_connect_ioctl(ep, arg);
    case CACT_SOCKCTL_LISTEN:
        return unix_listen_ioctl(ep, arg);
    case CACT_SOCKCTL_ACCEPT:
        return unix_accept_ioctl(ep, arg);
    case CACT_SOCKCTL_SHUTDOWN:
        return unix_shutdown_ioctl(ep, arg);
    case CACT_SOCKCTL_SETSOCKOPT:
    case CACT_SOCKCTL_GETSOCKOPT:
        return -EOPNOTSUPP;
    default:
        return -EINVAL;
    }
}
