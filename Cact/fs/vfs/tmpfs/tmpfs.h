#ifndef TMPFS_H
#define TMPFS_H

#include <stdint.h>
#include "vfs.h"

// Initialise tmpfs and return its root VFS node
void        tmpfs_init    (void);
vfs_node_t *tmpfs_get_root(void);

#endif