#ifndef MNTFS_H
#define MNTFS_H

#include <stdint.h>
#include "vfs.h"

#define MNTFS_NAME_LEN  128

typedef struct mntfs_entry mntfs_entry_t;
struct mntfs_entry {
    char             name[MNTFS_NAME_LEN];
    char             source[32];
    vfs_node_t      *target;
    int              persistent;
    mntfs_entry_t   *next;
};

typedef struct disk_entry disk_entry_t;
struct disk_entry {
    char          devname[32];    
    vfs_node_t   *ext4_root;   
    int           has_sys;    
    int           persistent; 

    vfs_node_t    disk_node;     
    vfs_node_t    sys_node;       
    vfs_node_t    raw_node;       

    disk_entry_t *next;
};

//Slave-диски для автомонтирования задаются статично в mntfs.c          
//в массиве _static_disks[]. Пересборка ядра после изменений

//Public api
void        mntfs_init(void);

int         mntfs_mount_disk  (const char *devname, vfs_node_t *ext4_root,
                                int persistent);
int         mntfs_umount_disk (const char *devname);

int         mntfs_mount  (const char *name, const char *source,
                           vfs_node_t *target, int persistent);
int         mntfs_umount (const char *name);

vfs_node_t *mntfs_get      (const char *name);
vfs_node_t *mntfs_get_root (void);
vfs_node_t *mntfs_get_ext4 (const char *devname);
vfs_node_t *mntfs_get_ext4_root(void);  
void        mntfs_list     (void);

vfs_node_t *mntfs_resolve_path  (const char *path);
int         mntfs_resolve_device(const char *devname,
                                  uint16_t *base_out, uint8_t *slave_out);

#endif 