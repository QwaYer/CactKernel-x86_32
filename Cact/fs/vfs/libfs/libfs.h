#ifndef LIBFS_H
#define LIBFS_H

#include <stdint.h>
#include "vfs.h"

// Initialise libfs, binding it to ext4's /lib directory
void        libfs_init    (vfs_node_t *ext4_root);

// Return the libfs root VFS node (to be registered in the mount table)
vfs_node_t *libfs_get_root(void);

#endif