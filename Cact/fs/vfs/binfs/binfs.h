#ifndef BINFS_H
#define BINFS_H

#include <stdint.h>
#include "vfs.h"

// Initialise binfs, binding it to ext4's /bin directory
void        binfs_init    (vfs_node_t *ext4_root);

// Return the binfs root VFS node (to be registered in the mount table)
vfs_node_t *binfs_get_root(void);

#endif