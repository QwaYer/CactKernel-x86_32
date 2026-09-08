#include "vfsdev.h"
#include "vfs.h"
#include "blkdev.h"
#include "fs_mod.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"

// vfsdev_mount.c — mount manager ("монтёр").
//
// A mount binds a filesystem living on a block device onto an existing
// directory in the VFS tree (Linux-style `mount /dev/<dev> <dir>`).  Mounts
// are manual only: nothing is auto-mounted when a device appears.  Each
// mounted device is tracked so umount and rescan guards know what is busy.

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef EBUSY
#define EBUSY 16
#endif

typedef struct vfsdev_mount vfsdev_mount_t;
struct vfsdev_mount {
    char             devname[32];
    char             target[128];
    char             fstype[32];
    vfsdev_mount_t  *next;
};

static vfsdev_mount_t *mount_list = 0;

// Strip a "/dev/" or leading "/" prefix from a device argument.
static const char *blk_name_from_arg(const char *p) {
    if (!p) return 0;
    if (p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v' && p[4] == '/')
        return p + 5;
    if (p[0] == '/' && p[1] != '\0')
        return p + 1;
    return p;
}

static vfsdev_mount_t *_find_dev(const char *devname) {
    for (vfsdev_mount_t *m = mount_list; m; m = m->next)
        if (streq(m->devname, devname)) return m;
    return 0;
}

static vfsdev_mount_t *_find_target(const char *target) {
    for (vfsdev_mount_t *m = mount_list; m; m = m->next)
        if (streq(m->target, target)) return m;
    return 0;
}

int vfsdev_device_mounted(const char *devname) {
    if (!devname) return 0;
    return _find_dev(devname) != 0;
}

int vfsdev_mount(const char *devarg, const char *target, const char *fstype) {
    if (!devarg || !target || !target[0] || !fstype || !fstype[0])
        return -EINVAL;

    const char *nm = blk_name_from_arg(devarg);
    if (!nm || !nm[0]) return -EINVAL;

    blkdev_t *bd = blkdev_find(nm);
    if (!bd) return -ENODEV;
    if (_find_dev(bd->name)) return -EBUSY;

    vfs_node_t *root = fs_mod_mount_type(bd, fstype);
    if (!root) return -ENODEV;

    char basename[128];
    vfs_node_t *parent = vfs_resolve_parent(target, basename, 128);
    if (!parent || !basename[0] || parent->type != VFS_DIRECTORY) {
        fs_mod_unmount_dev(bd);
        return -EINVAL;
    }

    if (vfs_mount(parent, basename, root) != 0) {
        fs_mod_unmount_dev(bd);
        return -EBUSY;
    }

    vfsdev_mount_t *m = (vfsdev_mount_t *)kmalloc(sizeof(vfsdev_mount_t));
    if (!m) {
        vfs_umount(parent, basename);
        fs_mod_unmount_dev(bd);
        return -1;
    }
    memset(m, 0, sizeof(vfsdev_mount_t));
    strlcpy(m->devname, bd->name, sizeof(m->devname));
    strlcpy(m->target,  target,  sizeof(m->target));
    strlcpy(m->fstype,  fstype,  sizeof(m->fstype));
    m->next = mount_list;
    mount_list = m;
    return 0;
}

int vfsdev_umount(const char *arg) {
    if (!arg) return -1;

    vfsdev_mount_t *m = _find_target(arg);
    if (!m) {
        const char *nm = blk_name_from_arg(arg);
        if (nm && nm[0]) m = _find_dev(nm);
    }
    if (!m) return -1;

    char basename[128];
    vfs_node_t *parent = vfs_resolve_parent(m->target, basename, 128);
    int r = -1;
    if (parent && basename[0])
        r = vfs_umount(parent, basename);

    blkdev_t *bd = blkdev_find(m->devname);
    if (bd) fs_mod_unmount_dev(bd);

    // Drop the tracking entry regardless of the vfs_umount result.
    vfsdev_mount_t **pp = &mount_list;
    while (*pp) {
        if (*pp == m) {
            *pp = m->next;
            kfree(m);
            break;
        }
        pp = &(*pp)->next;
    }
    return r;
}

void vfsdev_list(void) {
    printk("\nMounted block devices:\n");
    if (!mount_list) {
        printk("  (none)\n");
        return;
    }
    for (vfsdev_mount_t *m = mount_list; m; m = m->next) {
        printk("  ");
        printk(m->devname);
        printk("  ->  ");
        printk(m->target);
        printk("  [");
        printk(m->fstype);
        printk("]\n");
    }
}
