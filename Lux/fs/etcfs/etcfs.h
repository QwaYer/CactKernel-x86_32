#ifndef ETCFS_H
#define ETCFS_H

#include "vfs.h"
#include <stdint.h>

#define ETCFS_MAX_FILES   32
#define ETCFS_NAME_LEN   128
#define ETCFS_MAX_SIZE  4096

typedef struct {
    char             name[ETCFS_NAME_LEN];
    char*            data;
    uint32_t         size;
    int              dirty;
    int              used;
    struct vfs_node  node;   
} etcfs_entry_t;

void             etcfs_init   (struct vfs_node* ext4_node);
struct vfs_node* etcfs_get_root(void);
int              etcfs_read   (const char* name, char* buf, uint32_t size);
int              etcfs_write  (const char* name, const char* buf, uint32_t size);
void             etcfs_flush  (void);
int              etcfs_create (const char* name);
int              etcfs_delete (const char* name);

#endif