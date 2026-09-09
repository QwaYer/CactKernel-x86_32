#ifndef CACT_EPOLL_H
#define CACT_EPOLL_H

#include "vfs.h"

/* /dev/epoll factory: CACT_EPOLLCTL_CREATE returns an fd that multiplexes
 * readiness of other fds.  EPOLL_CTL_ADD/MOD/DEL and EPOLL_WAIT are handled
 * by the returned node's own ioctl op (level-triggered, over the kernel's
 * poll machinery). */
vfs_node_t *epoll_create_vnode(uint32_t flags);

#endif /* CACT_EPOLL_H */
