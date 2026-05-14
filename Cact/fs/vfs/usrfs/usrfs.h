#ifndef USRFS_H
#define USRFS_H

#include <stdint.h>
#include "vfs.h"

void        usrfs_init    (vfs_node_t *ext4_root);
vfs_node_t *usrfs_get_root(void);

#endif
