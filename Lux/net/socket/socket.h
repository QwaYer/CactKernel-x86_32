#ifndef KSOCKET_H
#define KSOCKET_H

#include <stdint.h>
#include "vfs.h"

/* ── Address families ─────────────────────────────────────────────────────── */
#define AF_INET     2

/* ── Socket types ─────────────────────────────────────────────────────────── */
#define SOCK_STREAM  1   /* TCP */
#define SOCK_DGRAM   2   /* UDP */

/* ── Protocol numbers ─────────────────────────────────────────────────────── */
#define IPPROTO_TCP   6
#define IPPROTO_UDP  17

/* ── sockaddr structures (shared with userspace via libc/include/socket.h) ── */
struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;   /* network byte order */
    uint32_t sin_addr;   /* network byte order */
    uint8_t  sin_zero[8];
};

/* ── args structs for sendto / recvfrom (passed via ebx pointer) ──────────── */
typedef struct {
    int                       fd;
    const void               *buf;
    uint32_t                  len;
    int                       flags;
    const struct sockaddr_in *dest;
    uint32_t                  addrlen;
} sendto_args_t;

typedef struct {
    int                  fd;
    void                *buf;
    uint32_t             len;
    int                  flags;
    struct sockaddr_in  *src;
    uint32_t            *addrlen;
} recvfrom_args_t;

/* ── Kernel socket handle ─────────────────────────────────────────────────── */
#define KSOCK_MAX  16

typedef enum {
    KS_NONE = 0,
    KS_TCP,
    KS_UDP,
} ksock_kind_t;

typedef struct ksock {
    uint8_t      used;
    ksock_kind_t kind;
    int          proto_idx;  /* index into tcp_sockets[] or udp_socks[] */
} ksock_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Initialise socket subsystem */
void ksock_init(void);

/* Create a new socket, returns a VFS node (type VFS_SOCKET) or NULL */
vfs_node_t *ksock_create(int domain, int type, int protocol);

/* Extract ksock from a VFS socket node */
ksock_t *ksock_from_node(vfs_node_t *node);

/* Accept a pending TCP connection on a LISTEN socket.
   Fills *peer_out if non-NULL.  Returns new VFS node or NULL if none ready. */
vfs_node_t *ksock_tcp_accept(vfs_node_t *listen_node,
                              struct sockaddr_in *peer_out);

#endif /* KSOCKET_H */
