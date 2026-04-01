#ifndef _SOCKET_H
#define _SOCKET_H

#include <stdint.h>
#include "syscall.h"

/* ── Address families ─────────────────────────────────────────────────────── */
#define AF_INET     2

/* ── Socket types ─────────────────────────────────────────────────────────── */
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

/* ── Protocol numbers ─────────────────────────────────────────────────────── */
#define IPPROTO_TCP   6
#define IPPROTO_UDP  17

/* ── INADDR_ANY ───────────────────────────────────────────────────────────── */
#define INADDR_ANY  0

/* ── shutdown(2) — how values ─────────────────────────────────────────────── */
#define SHUT_RD    0
#define SHUT_WR    1
#define SHUT_RDWR  2

/* ── Socket option levels ─────────────────────────────────────────────────── */
#define SOL_SOCKET   1
/* IPPROTO_TCP (6) also used as option level */

/* ── SOL_SOCKET option names ──────────────────────────────────────────────── */
#define SO_REUSEADDR  2
#define SO_KEEPALIVE  9
#define SO_ERROR      4

/* ── IPPROTO_TCP option names ─────────────────────────────────────────────── */
#define TCP_NODELAY   1

/* ── sockaddr structures ──────────────────────────────────────────────────── */
struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;   /* network byte order — use htons() */
    uint32_t sin_addr;   /* network byte order — use htonl() */
    uint8_t  sin_zero[8];
};

/* ── Byte-order helpers ───────────────────────────────────────────────────── */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF)
         | ((x >>  8) & 0xFF00)
         | ((x <<  8) & 0xFF0000)
         | ((x << 24) & 0xFF000000);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* ── setsockopt / getsockopt args structs (passed via pointer to kernel) ──── */
typedef struct {
    int         fd;
    int         level;
    int         optname;
    const void *optval;
    uint32_t    optlen;
} setsockopt_args_t;

typedef struct {
    int       fd;
    int       level;
    int       optname;
    void     *optval;
    uint32_t *optlen;
} getsockopt_args_t;

/* ── sendto / recvfrom args struct (mirrors the kernel's definition) ──────── */
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

/* ── Public API ───────────────────────────────────────────────────────────── */

int socket    (int domain, int type, int protocol);
int bind      (int fd, const struct sockaddr_in *addr, uint32_t addrlen);
int connect   (int fd, const struct sockaddr_in *addr, uint32_t addrlen);
int listen    (int fd, int backlog);
int accept    (int fd, struct sockaddr_in *peer, uint32_t *addrlen);
int send      (int fd, const void *buf, uint32_t len, int flags);
int recv      (int fd, void *buf, uint32_t len, int flags);
int sendto    (int fd, const void *buf, uint32_t len, int flags,
               const struct sockaddr_in *dest, uint32_t addrlen);
int recvfrom  (int fd, void *buf, uint32_t len, int flags,
               struct sockaddr_in *src, uint32_t *addrlen);
int shutdown  (int fd, int how);
int setsockopt(int fd, int level, int optname,
               const void *optval, uint32_t optlen);
int getsockopt(int fd, int level, int optname,
               void *optval, uint32_t *optlen);

#endif /* _SOCKET_H */
