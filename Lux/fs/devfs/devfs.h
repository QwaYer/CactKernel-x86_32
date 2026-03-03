#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include "vfs.h"

#define DEVFS_MAX_DEVICES   64
#define DEVFS_MAGIC         0x44455646

typedef struct devfs_entry {
    char            name[128];
    struct vfs_node node;
    int             used;
} devfs_entry_t;


//Public api
void             devfs_init(void);
struct vfs_node* devfs_register(const char* name, struct vfs_node* node);
int              devfs_unregister(const char* name);
struct vfs_node* devfs_register_fifo(struct vfs_node* node);
struct vfs_node* devfs_get_root(void);

#endif