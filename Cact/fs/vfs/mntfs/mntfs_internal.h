#ifndef MNTFS_INTERNAL_H
#define MNTFS_INTERNAL_H

#include "mntfs.h"

/* mntfs.c — core state shared with the mounts/ops modules. */
extern mntfs_entry_t *mnt_list;
extern disk_entry_t  *disk_list;
extern vfs_node_t     mntfs_root;
extern char           boot_devname[32];

mntfs_entry_t *_find(const char *name);
disk_entry_t  *_find_disk(const char *devname);
int            _sys_pfx(disk_entry_t *d, char *pfx);

/* mntfs_ops.c — VFS ops tables for root/disk/sys/raw directories. */
extern vfs_ops_t raw_ops;
extern vfs_ops_t sys_ops;
extern vfs_ops_t disk_ops;
extern vfs_ops_t root_ops;

/* mntfs_mounts.c — /etc/mounts persistence. */
void _mounts_add(const char *devname);
void _mounts_remove(const char *devname);
void _mounts_mount_all(void);

#endif
