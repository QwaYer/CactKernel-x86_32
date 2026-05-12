#ifndef SBINFS_H
#define SBINFS_H

#include <stdint.h>
#include "vfs.h"

void        sbinfs_init    (vfs_node_t *ext4_root);
vfs_node_t *sbinfs_get_root(void);

#endif
