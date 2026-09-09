#ifndef CACT_EVENTFD_H
#define CACT_EVENTFD_H

#include "vfs.h"

/* /dev/eventfd factory: CACT_EVENTFDCTL_CREATE returns an fd backed by a
 * counter node (read = fetch+reset, write = add, poll = readable when > 0). */
vfs_node_t *eventfd_create_vnode(uint32_t initval, uint32_t flags);

#endif /* CACT_EVENTFD_H */
