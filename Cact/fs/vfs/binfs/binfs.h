#ifndef BINFS_H
#define BINFS_H

#include <stdint.h>
#include "vfs.h"

// Public API 
void        binfs_init    (vfs_node_t *ext4_root);
vfs_node_t *binfs_get_root(void);

#endif