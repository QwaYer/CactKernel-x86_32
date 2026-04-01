#ifndef _SELECT_H
#define _SELECT_H

#include <stdint.h>
#include "time.h"    /* struct timeval */
#include "syscall.h" /* SYS_SELECT, __syscall1 */

/* fd_set covers all MAX_FD=256 descriptors */
#define FD_SETSIZE 256

typedef struct {
    uint32_t fds_bits[FD_SETSIZE / 32];
} fd_set;

#define FD_ZERO(s) \
    do { for (int _i = 0; _i < FD_SETSIZE/32; _i++) (s)->fds_bits[_i] = 0; } while (0)
#define FD_SET(fd, s)   ((s)->fds_bits[(fd)/32] |=  (1u << ((fd) % 32)))
#define FD_CLR(fd, s)   ((s)->fds_bits[(fd)/32] &= ~(1u << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(fd)/32] >> ((fd) % 32)) & 1u)

/* Packed args struct – passed as a single pointer in ebx to SYS_SELECT */
typedef struct {
    int             nfds;
    fd_set         *readfds;
    fd_set         *writefds;
    fd_set         *exceptfds;
    struct timeval *timeout;   /* NULL = block forever; {0,0} = non-blocking */
} select_args_t;

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

#endif /* _SELECT_H */
