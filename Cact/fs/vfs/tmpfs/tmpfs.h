#ifndef TMPFS_H
#define TMPFS_H

#include <stdint.h>
#include "vfs.h"

// Create a new RAM-filesystem instance rooted at `name` (heap-allocated).
// Used for the rootfs "/" when no boot filesystem is available.
vfs_node_t *tmpfs_create_root(const char *name);

// Initialise tmpfs and return its root VFS node
void        tmpfs_init    (void);
vfs_node_t *tmpfs_get_root(void);

#endif