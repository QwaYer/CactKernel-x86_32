#ifndef MNTFS_H
#define MNTFS_H

#include "vfs.h"
#include <stdint.h>

#define MNTFS_MAX_MOUNTS  16
#define MNTFS_NAME_LEN   128

typedef struct {
    char             name[MNTFS_NAME_LEN]; 
    struct vfs_node* target;
    int              used;
    char             source[32];           
    int              persistent;            
} mntfs_entry_t;


//Public api
void             mntfs_init(void);

int              mntfs_mount(const char* name, const char* source,
                              struct vfs_node* target, int persistent);

int              mntfs_umount(const char* name);

struct vfs_node* mntfs_get(const char* name);
struct vfs_node* mntfs_get_root(void);
void             mntfs_list(void);

struct vfs_node* mntfs_resolve_path(const char* path);

int              mntfs_resolve_device(const char* devname,
                                       uint16_t* base_out,
                                       uint8_t*  slave_out);

#endif