#ifndef _POLL_H
#define _POLL_H

#include <stdint.h>
#include "syscall.h" /* SYS_POLL, __syscall3 */

/* Requested / returned events */
#define POLLIN   0x0001   /* data available to read              */
#define POLLOUT  0x0004   /* space available to write            */
#define POLLERR  0x0008   /* error condition (revents only)      */
#define POLLHUP  0x0010   /* hang-up (revents only)              */
#define POLLNVAL 0x0020   /* invalid fd (revents only)           */

struct pollfd {
    int   fd;       /* file descriptor to watch            */
    short events;   /* events to wait for (POLLIN|POLLOUT) */
    short revents;  /* events that occurred (filled by kernel) */
};

/* timeout_ms: -1 = block forever, 0 = non-blocking, >0 = ms deadline */
int poll(struct pollfd *fds, int nfds, int timeout_ms);

#endif /* _POLL_H */
