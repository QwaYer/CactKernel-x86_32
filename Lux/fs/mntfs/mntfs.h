#ifndef MNTFS_H
#define MNTFS_H

#include "vfs.h"


#define MNTFS_MAX_MOUNTS  16
#define MNTFS_NAME_LEN   128

typedef struct {
    char             name[MNTFS_NAME_LEN];  /* mount point name   */
    struct vfs_node* target;                 /* mounted filesystem */
    int              used;
} mntfs_entry_t;

void             mntfs_init(void);

int              mntfs_mount(const char* name, struct vfs_node* target);

int              mntfs_umount(const char* name);

struct vfs_node* mntfs_get(const char* name);

struct vfs_node* mntfs_get_root(void);

void             mntfs_list(void);

#endif 