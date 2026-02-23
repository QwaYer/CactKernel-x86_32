#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

void             procfs_init(void);
struct vfs_node* procfs_get_root(void);

#endif