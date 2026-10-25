#ifndef CACT_EVENTFD_H
#define CACT_EVENTFD_H

#include "vfs.h"

/* /dev/eventfd factory: CACT_EVENTFDCTL_CREATE returns an fd backed by a
 * counter node (read = fetch+reset, write = add, poll = readable when > 0). */
vfs_node_t *eventfd_create_vnode(uint32_t initval, uint32_t flags);

/* In-kernel use (the DRM syncobj EVENTFD registration).
 *
 * eventfd_node_get() resolves `fd` in the calling task's table and takes a
 * reference, so the caller may hold the eventfd after the fd is closed:
 *   0 on success (the node is written to *out), -EBADF when the fd is not
 *   open, -EINVAL when it is open but is not an eventfd.
 * eventfd_node_put() drops that reference and frees the node at zero.
 * eventfd_node_signal() adds to the counter (the write(2) increment, without
 * blocking: it saturates instead of overflowing). */
int  eventfd_node_get(int fd, vfs_node_t **out);
void eventfd_node_put(vfs_node_t *node);
int  eventfd_node_signal(vfs_node_t *node, uint64_t n);

#endif /* CACT_EVENTFD_H */
