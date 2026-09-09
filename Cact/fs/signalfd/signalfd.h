#ifndef CACT_SIGNALFD_H
#define CACT_SIGNALFD_H

#include "vfs.h"

/* /dev/signalfd factory: CACT_SIGNALFDCTL_CREATE returns an fd that reports
 * the calling process's pending blocked signals from its mask.  The mask of
 * an existing signalfd is updated with CACT_SIGNALFD_SETMASK. */
vfs_node_t *signalfd_create_vnode(uint32_t mask, uint32_t flags);

#endif /* CACT_SIGNALFD_H */
