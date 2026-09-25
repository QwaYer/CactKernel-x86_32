#ifndef VARFS_H
#define VARFS_H

#include <stdint.h>
#include "vfs.h"

/* varfs — a dedicated writable namespace for the runtime data of user
 * services (logs, local state). Ring-3 init (cgoct and the like) must not
 * create /var and /var/log itself: the kernel guarantees they exist on
 * boot. Services merely write into the already prepared structure.
 *
 * Implementation layer: forward to ext4 /var (if there is a boot-disk). In
 * nodisk mode this is a noop — nothing can be written, but the mount point is
 * still present in the VFS root, so that paths from userspace do not fail on
 * walk. */

/* Initialise varfs and ensure /var (+ /var/log) on the backing ext4 root. */
void        varfs_init    (vfs_node_t *ext4_root);

/* Return the varfs root node for registering in the mount table. */
vfs_node_t *varfs_get_root(void);

#endif
