#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "memory.h"
#include "kernel.h"

/* ── Global socket table ──────────────────────────────────────────────────── */

static ksock_t ksock_table[KSOCK_MAX];

void ksock_init(void) {
    kprint("[ksock] clearing kernel socket table (16 slots)\n");
    for (int i = 0; i < KSOCK_MAX; i++)
        ksock_table[i].used = 0;
    klog(LOG_OK, "kernel socket table ready — 16 slots");
}

/* ── Forward declarations for VFS ops ────────────────────────────────────── */

static int  socket_read_op (struct vfs_node *node, uint32_t off,
                             uint32_t size, char *buf);
static int  socket_write_op(struct vfs_node *node, uint32_t off,
                             uint32_t size, char *buf);
static void socket_close_op(struct vfs_node *node);

static vfs_ops_t socket_ops = {
    .read   = socket_read_op,
    .write  = socket_write_op,
    .close  = socket_close_op,
    /* everything else is NULL — not meaningful for sockets */
};

/* ── Helpers ──────────────────────────────────────────────────────────────── */

ksock_t *ksock_from_node(vfs_node_t *node) {
    if (!node || node->type != VFS_SOCKET) return 0;
    return (ksock_t *)node->priv;
}

static ksock_t *ksock_alloc(void) {
    for (int i = 0; i < KSOCK_MAX; i++) {
        if (!ksock_table[i].used) {
            ksock_table[i].used = 1;
            return &ksock_table[i];
        }
    }
    return 0;
}

static vfs_node_t *make_socket_node(ksock_t *ks) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;

    /* zero-init */
    for (uint32_t i = 0; i < sizeof(vfs_node_t); i++)
        ((uint8_t *)node)[i] = 0;

    node->type = VFS_SOCKET;
    node->ops  = &socket_ops;
    node->priv = ks;
    return node;
}

/* ── Create ───────────────────────────────────────────────────────────────── */

vfs_node_t *ksock_create(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return 0;

    ksock_t *ks = ksock_alloc();
    if (!ks) return 0;

    if (type == SOCK_STREAM) {
        int idx = tcp_socket();
        if (idx < 0) { ks->used = 0; return 0; }
        ks->kind      = KS_TCP;
        ks->proto_idx = idx;
    } else if (type == SOCK_DGRAM) {
        int idx = udp_sock_alloc();
        if (idx < 0) { ks->used = 0; return 0; }
        ks->kind      = KS_UDP;
        ks->proto_idx = idx;
    } else {
        ks->used = 0;
        return 0;
    }

    vfs_node_t *node = make_socket_node(ks);
    if (!node) {
        if (ks->kind == KS_TCP) tcp_close(ks->proto_idx);
        else                    udp_sock_free(ks->proto_idx);
        ks->used = 0;
    }
    return node;
}

/* ── Accept ───────────────────────────────────────────────────────────────── */

vfs_node_t *ksock_tcp_accept(vfs_node_t *listen_node,
                              struct sockaddr_in *peer_out) {
    ksock_t *lks = ksock_from_node(listen_node);
    if (!lks || lks->kind != KS_TCP) return 0;

    int listen_proto = lks->proto_idx;

    /* Search for a TCP socket that completed handshake on this listener */
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &tcp_sockets[i];
        if (!s->used)               continue;
        if (!s->accept_ready)       continue;
        if (s->listen_parent != (int8_t)listen_proto) continue;

        s->accept_ready = 0;  /* consume */

        /* Fill peer address */
        if (peer_out) {
            peer_out->sin_family = AF_INET;
            peer_out->sin_port   = htons(s->remote_port);
            peer_out->sin_addr   = s->remote_ip;  /* already network order */
        }

        /* Wrap in a new ksock + vfs node */
        ksock_t *ks = ksock_alloc();
        if (!ks) return 0;
        ks->kind      = KS_TCP;
        ks->proto_idx = i;

        vfs_node_t *node = make_socket_node(ks);
        if (!node) { ks->used = 0; return 0; }
        return node;
    }
    return 0;
}

/* ── VFS ops ──────────────────────────────────────────────────────────────── */

static int socket_read_op(struct vfs_node *node, uint32_t off,
                           uint32_t size, char *buf) {
    (void)off;
    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;
    if (ks->shutdown_rd) return -1;  /* read direction shut down */

    if (ks->kind == KS_TCP)
        return tcp_recv(ks->proto_idx, (uint8_t *)buf, (uint16_t)size);

    if (ks->kind == KS_UDP)
        return udp_sock_recv(ks->proto_idx, (uint8_t *)buf, (uint16_t)size,
                             0, 0);
    return -1;
}

static int socket_write_op(struct vfs_node *node, uint32_t off,
                            uint32_t size, char *buf) {
    (void)off;
    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;
    if (ks->shutdown_wr) return -1;  /* write direction shut down */

    if (ks->kind == KS_TCP)
        return tcp_send(ks->proto_idx, (uint8_t *)buf, (uint16_t)size);

    /* UDP write without explicit dest is not supported; use sendto */
    return -1;
}

static void socket_close_op(struct vfs_node *node) {
    ksock_t *ks = ksock_from_node(node);
    if (!ks) return;

    if (ks->kind == KS_TCP) tcp_close(ks->proto_idx);
    if (ks->kind == KS_UDP) udp_sock_free(ks->proto_idx);

    ks->used = 0;
    /* The VFS node itself was kmalloc'd; free it. */
    kfree_heap(node);
}

/* ── shutdown ─────────────────────────────────────────────────────────────── */

int ksock_shutdown(vfs_node_t *node, int how) {
    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -1;

    if (how == SHUT_RD || how == SHUT_RDWR)
        ks->shutdown_rd = 1;

    if (how == SHUT_WR || how == SHUT_RDWR) {
        if (!ks->shutdown_wr) {
            ks->shutdown_wr = 1;
            /* For TCP, send FIN to peer (half-close the send stream) */
            if (ks->kind == KS_TCP)
                tcp_shutdown_wr(ks->proto_idx);
        }
    }

    return 0;
}
