#ifndef MNTFS_H
#define MNTFS_H

#include <stdint.h>
#include "vfs.h"

#define MNTFS_NAME_LEN  128

// A virtual mount point (e.g. devfs, procfs) under a disk's sys/ directory
typedef struct mntfs_entry mntfs_entry_t;
struct mntfs_entry {
    char             name[MNTFS_NAME_LEN];
    char             source[32];       // "devfs", "procfs", etc.
    vfs_node_t      *target;           // the mounted VFS root
    int              persistent;       // saved to /etc/mounts?
    mntfs_entry_t   *next;
};

// A physical disk (master or slave)
typedef struct disk_entry disk_entry_t;
struct disk_entry {
    char          devname[32];         // block device name (e.g. "nvme0")
    vfs_node_t   *ext4_root;          // ext4 root node (raw/)
    int           has_sys;            // master disk has sys/ with virtual mounts
    int           persistent;         // saved to /etc/mounts?

    vfs_node_t    disk_node;          // /<devname> directory
    vfs_node_t    sys_node;           // /<devname>/sys directory
    vfs_node_t    raw_node;           // /<devname>/raw directory

    disk_entry_t *next;
};

// Initialise mntfs: mount boot disk, set up VFS root, mount all subsystems
void        mntfs_init(void);

// Mount/unmount a physical disk
int         mntfs_mount_disk  (const char *devname, vfs_node_t *ext4_root,
                                int persistent);
int         mntfs_umount_disk (const char *devname);

// Mount/unmount a virtual mount point
int         mntfs_mount  (const char *name, const char *source,
                           vfs_node_t *target, int persistent);
int         mntfs_umount (const char *name);

// Lookups
vfs_node_t *mntfs_get      (const char *name);
vfs_node_t *mntfs_get_root (void);
vfs_node_t *mntfs_get_ext4 (const char *devname);
vfs_node_t *mntfs_get_ext4_root(void);
void        mntfs_list     (void);

// Path resolution helpers
vfs_node_t *mntfs_resolve_path  (const char *path);
int         mntfs_resolve_device(const char *devname, uint32_t *dev_out);

#endif