#include "socket.h"
#include "syscall.h"

int socket(int domain, int type, int protocol) {
    return __syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const struct sockaddr_in *addr, uint32_t addrlen) {
    return __syscall3(SYS_BIND, fd, (int)addr, (int)addrlen);
}

int connect(int fd, const struct sockaddr_in *addr, uint32_t addrlen) {
    return __syscall3(SYS_CONNECT, fd, (int)addr, (int)addrlen);
}

int listen(int fd, int backlog) {
    return __syscall2(SYS_LISTEN, fd, backlog);
}

int accept(int fd, struct sockaddr_in *peer, uint32_t *addrlen) {
    return __syscall3(SYS_ACCEPT, fd, (int)peer, (int)addrlen);
}

int send(int fd, const void *buf, uint32_t len, int flags) {
    /* flags not yet honoured by kernel — pass len in edx, flags ignored */
    (void)flags;
    return __syscall3(SYS_SEND, fd, (int)buf, (int)len);
}

int recv(int fd, void *buf, uint32_t len, int flags) {
    (void)flags;
    return __syscall3(SYS_RECV, fd, (int)buf, (int)len);
}

int sendto(int fd, const void *buf, uint32_t len, int flags,
           const struct sockaddr_in *dest, uint32_t addrlen) {
    sendto_args_t args = {
        .fd     = fd,
        .buf    = buf,
        .len    = len,
        .flags  = flags,
        .dest   = dest,
        .addrlen= addrlen,
    };
    return __syscall1(SYS_SENDTO, (int)&args);
}

int recvfrom(int fd, void *buf, uint32_t len, int flags,
             struct sockaddr_in *src, uint32_t *addrlen) {
    recvfrom_args_t args = {
        .fd      = fd,
        .buf     = buf,
        .len     = len,
        .flags   = flags,
        .src     = src,
        .addrlen = addrlen,
    };
    return __syscall1(SYS_RECVFROM, (int)&args);
}

int shutdown(int fd, int how) {
    return __syscall2(SYS_SHUTDOWN, fd, how);
}

int setsockopt(int fd, int level, int optname,
               const void *optval, uint32_t optlen) {
    setsockopt_args_t args = {
        .fd      = fd,
        .level   = level,
        .optname = optname,
        .optval  = optval,
        .optlen  = optlen,
    };
    return __syscall1(SYS_SETSOCKOPT, (int)&args);
}

int getsockopt(int fd, int level, int optname,
               void *optval, uint32_t *optlen) {
    getsockopt_args_t args = {
        .fd      = fd,
        .level   = level,
        .optname = optname,
        .optval  = optval,
        .optlen  = optlen,
    };
    return __syscall1(SYS_GETSOCKOPT, (int)&args);
}
