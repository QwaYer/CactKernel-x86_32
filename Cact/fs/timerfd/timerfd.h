#ifndef CACT_TIMERFD_H
#define CACT_TIMERFD_H

#include "vfs.h"

/* /dev/timerfd factory: CACT_TIMERFDCTL_CREATE returns an fd backed by a
 * periodic/one-shot monotonic timer (100 Hz tick).  CACT_TIMERFD_SETTIME /
 * CACT_TIMERFD_GETTIME are handled by the returned node's own ioctl op. */
vfs_node_t *timerfd_create_vnode(int32_t clockid, uint32_t flags);

#endif /* CACT_TIMERFD_H */
