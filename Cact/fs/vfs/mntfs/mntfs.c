#include "mntfs.h"
#include "mntfs_internal.h"
#include "etcfs.h"
#include "devfs.h"
#include "procfs.h"
#include "tmpfs.h"
#include "binfs.h"
#include "sbinfs.h"
#include "libfs.h"
#include "varfs.h"
#include "usrfs.h"
#include "vfs.h"
#include "fs_mod.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "blkdev.h"

// Global linked lists
mntfs_entry_t  *mnt_list   = 0;    // virtual mount points (sys/...)
disk_entry_t   *disk_list  = 0;    // physical disks (raw + sys)
vfs_node_t      mntfs_root;
int             mntfs_ready = 0;
char            boot_devname[32];   // name of the boot block device

/* Fallback layout when no boot disk/ext4 is available. */
static void mntfs_setup_nodisk(void) {
    devfs_init();
    procfs_init();
    tmpfs_init();
    etcfs_init(0);
    binfs_init(0);
    sbinfs_init(0);
    libfs_init(0);
    varfs_init(0);
    usrfs_init(0);

    extern int vfs_mount(vfs_node_t *host, const char *name, vfs_node_t *target);
    vfs_mount(vfs_root, "bin",  binfs_get_root());
    vfs_mount(vfs_root, "sbin", sbinfs_get_root());
    vfs_mount(vfs_root, "lib",  libfs_get_root());
    vfs_mount(vfs_root, "usr",  usrfs_get_root());
    vfs_mount(vfs_root, "dev",  devfs_get_root());
    vfs_mount(vfs_root, "proc", procfs_get_root());
    vfs_mount(vfs_root, "tmp",  tmpfs_get_root());
    vfs_mount(vfs_root, "etc",  etcfs_get_root());
    vfs_mount(vfs_root, "var",  varfs_get_root());
}

// Find a mount point by full path
mntfs_entry_t *_find(const char *name) {
    for (mntfs_entry_t *e = mnt_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
}

// Find a disk entry by device name
disk_entry_t *_find_disk(const char *devname) {
    for (disk_entry_t *d = disk_list; d; d = d->next)
        if (streq(d->devname, devname)) return d;
    return 0;
}

// Build per-disk sys prefix ("<disk>/sys/").
int _sys_pfx(disk_entry_t *d, char *pfx) {
    strlcpy(pfx, d->devname, MNTFS_NAME_LEN);
    int l = 0; while (pfx[l]) l++;
    pfx[l++]='/'; pfx[l++]='s'; pfx[l++]='y'; pfx[l++]='s'; pfx[l++]='/';
    pfx[l] = '\0';
    return l;
}

// Resolve a device name to its blkdev id (uses blkdev_find)
int mntfs_resolve_device(const char *devname, uint32_t *dev_out) {
    blkdev_t *bd = blkdev_find(devname);
    if (bd) {
        if (dev_out) *dev_out = bd->id;
        return 0;
    }
    return -1;
}

// Convenience wrappers
vfs_node_t *mntfs_resolve_path(const char *path) { return vfs_walk_path(vfs_root,path); }
vfs_node_t *mntfs_get_root(void)                  { return &mntfs_root; }
vfs_node_t *mntfs_get_ext4_root(void)             { return disk_list ? disk_list->ext4_root : 0; }
vfs_node_t *mntfs_get_ext4(const char *devname)   {
    disk_entry_t *d=_find_disk(devname); return d ? d->ext4_root : 0;
}
vfs_node_t *mntfs_get(const char *name) {
    mntfs_entry_t *e=_find(name); return e ? e->target : 0;
}

// Register a physical disk with its ext4 root
int mntfs_mount_disk(const char *devname, vfs_node_t *ext4_root, int persistent) {    if (!devname||!ext4_root) return -1;
    if (_find_disk(devname)) return -1;

    disk_entry_t *d=(disk_entry_t*)kmalloc(sizeof(disk_entry_t));
    if (!d) return -1;
    memset(d,0,sizeof(disk_entry_t));

    strlcpy(d->devname,devname,32);
    d->ext4_root  = ext4_root;
    d->persistent = persistent;
    d->has_sys    = 0;

    strlcpy(d->disk_node.name,devname,128);
    d->disk_node.type=VFS_DIRECTORY; d->disk_node.ops=&disk_ops; d->disk_node.priv=d;

    strlcpy(d->sys_node.name,"sys",128);
    d->sys_node.type=VFS_DIRECTORY; d->sys_node.ops=&sys_ops; d->sys_node.priv=d;

    strlcpy(d->raw_node.name,"raw",128);
    d->raw_node.type=VFS_DIRECTORY; d->raw_node.ops=&raw_ops; d->raw_node.priv=d;

    if (!disk_list) { disk_list=d; }
    else { disk_entry_t *t=disk_list; while(t->next) t=t->next; t->next=d; }

    if (persistent) _mounts_add(devname);
    return 0;
}

// Expose a filesystem root on a block device (whole disk or partition).
int mntfs_mount_blkdev(blkdev_t *bd, vfs_node_t *root, int persistent) {
    if (!bd || !root) return -1;
    if (_find_disk(bd->name)) return -1;   // already mounted
    return mntfs_mount_disk(bd->name, root, persistent);
}

int mntfs_unmount_blkdev(blkdev_t *bd) {
    if (!bd) return -1;
    return mntfs_umount_disk(bd->name);
}

int mntfs_device_mounted(const char *devname) {
    return _find_disk(devname) != 0;
}

// Register a virtual mount point (e.g. devfs, procfs) under a disk's sys/
int mntfs_mount(const char *name, const char *source,
                 vfs_node_t *target, int persistent) {
    if (!name||!target) return -1;
    if (_find(name)) return -1;
    mntfs_entry_t *e=(mntfs_entry_t*)kmalloc(sizeof(mntfs_entry_t));
    if (!e) return -1;
    memset(e,0,sizeof(mntfs_entry_t));
    strlcpy(e->name,name,MNTFS_NAME_LEN);
    strlcpy(e->source,source?source:"",32);
    e->target=target; e->persistent=persistent;
    e->next=mnt_list; mnt_list=e;
    return 0;
}

// Unmount a physical disk
int mntfs_umount_disk(const char *devname) {
    if (!devname) return -1;
    disk_entry_t *d=_find_disk(devname);
    if (!d) return -1;
    if (d->has_sys) return -2;   // master disk cannot be unmounted while sys/ is active
    disk_entry_t **pp=&disk_list;
    while (*pp) {
        if (streq((*pp)->devname,devname)) {
            disk_entry_t *dead=*pp; *pp=dead->next;
            if (dead->persistent) _mounts_remove(dead->devname);
            kfree(dead); return 0;
        }
        pp=&(*pp)->next;
    }
    return -1;
}

// Unmount a virtual mount point
int mntfs_umount(const char *name) {
    if (!name) return -1;
    mntfs_entry_t **pp=&mnt_list;
    while (*pp) {
        if (streq((*pp)->name,name)) {
            mntfs_entry_t *dead=*pp; *pp=dead->next;
            kfree(dead); return 0;
        }
        pp=&(*pp)->next;
    }
    return -1;
}

// Print all mounted disks and virtual mount points
void mntfs_list(void) {
    printk("\nDisks:\n");
    int any=0;
    for (disk_entry_t *d=disk_list;d;d=d->next) {
        printk("  /"); printk(d->devname);
        printk(d->has_sys ? "  [master: sys+raw]" : "  [slave: raw]");
        printk(d->persistent ? "  [saved]\n" : "\n");
        any=1;
    }
    if (!any) printk("  (none)\n");
}

// Initialize mntfs, boot disk, and subsystem mounts.
void mntfs_init(void) {
    if (mntfs_ready) return;

    memset(&mntfs_root,0,sizeof(vfs_node_t));
    strlcpy(mntfs_root.name,"/",128);
    mntfs_root.type=VFS_DIRECTORY;
    mntfs_root.ops=&root_ops;
    vfs_root=&mntfs_root;
    mntfs_ready=1;

    blkdev_t *boot = blkdev_get_boot();
    if (!boot) {
        pr_warn("  %-11s : no boot block device — nodisk root "
                "(load ahci/nvme kmod for disk)\n", "mntfs");
        mntfs_setup_nodisk();
        return;
    }

    strlcpy(boot_devname, boot->name, 32);
    vfs_node_t *ext4 = fs_mod_mount(boot);
    if (!ext4) {
        pr_warn("  %-11s : ext4 mount failed on %s — nodisk root\n",
                "mntfs", boot_devname);
        mntfs_setup_nodisk();
        return;
    }
    mntfs_mount_disk(boot_devname, ext4, 0);
    disk_entry_t *boot_disk = _find_disk(boot_devname);
    boot_disk->has_sys = 1;   // master disk gets sys/ virtual directory

    // Mount subsystem roots under <boot>/sys.
    etcfs_init(ext4);
    char sys_etc[64];
    strlcpy(sys_etc, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/etc\n");
    } else {
        strlcpy(sys_etc + strlen(boot_devname), "/sys/etc", 64 - strlen(boot_devname));
        mntfs_mount(sys_etc, "etcfs", etcfs_get_root(), 0);
    }

    _mounts_mount_all();

    devfs_init();
    char sys_dev[64];
    strlcpy(sys_dev, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/dev\n");
    } else {
        strlcpy(sys_dev + strlen(boot_devname), "/sys/dev", 64 - strlen(boot_devname));
        mntfs_mount(sys_dev, "devfs", devfs_get_root(), 0);
    }

    procfs_init();
    char sys_proc[64];
    strlcpy(sys_proc, boot_devname, 64);
    if (strlen(boot_devname) + 10 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/proc\n");
    } else {
        strlcpy(sys_proc + strlen(boot_devname), "/sys/proc", 64 - strlen(boot_devname));
        mntfs_mount(sys_proc, "procfs", procfs_get_root(), 0);
    }

    tmpfs_init();
    char sys_tmp[64];
    strlcpy(sys_tmp, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/tmp\n");
    } else {
        strlcpy(sys_tmp + strlen(boot_devname), "/sys/tmp", 64 - strlen(boot_devname));
        mntfs_mount(sys_tmp, "tmpfs", tmpfs_get_root(), 0);
    }

    binfs_init(ext4);
    char sys_bin[64];
    strlcpy(sys_bin, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/bin\n");
    } else {
        strlcpy(sys_bin + strlen(boot_devname), "/sys/bin", 64 - strlen(boot_devname));
        mntfs_mount(sys_bin, "binfs", binfs_get_root(), 0);
    }

    sbinfs_init(ext4);
    char sys_sbin[64];
    strlcpy(sys_sbin, boot_devname, 64);
    if (strlen(boot_devname) + 10 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/sbin\n");
    } else {
        strlcpy(sys_sbin + strlen(boot_devname), "/sys/sbin", 64 - strlen(boot_devname));
        mntfs_mount(sys_sbin, "sbinfs", sbinfs_get_root(), 0);
    }

    libfs_init(ext4);
    char sys_lib[64];
    strlcpy(sys_lib, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/lib\n");
    } else {
        strlcpy(sys_lib + strlen(boot_devname), "/sys/lib", 64 - strlen(boot_devname));
        mntfs_mount(sys_lib, "libfs", libfs_get_root(), 0);
    }

    varfs_init(ext4);
    char sys_var[64];
    strlcpy(sys_var, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/var\n");
    } else {
        strlcpy(sys_var + strlen(boot_devname), "/sys/var", 64 - strlen(boot_devname));
        mntfs_mount(sys_var, "varfs", varfs_get_root(), 0);
    }

    usrfs_init(ext4);
    char sys_usr[64];
    strlcpy(sys_usr, boot_devname, 64);
    if (strlen(boot_devname) + 9 > 63) {
        printk("[mntfs] boot_devname too long, skipping /sys/usr\n");
    } else {
        strlcpy(sys_usr + strlen(boot_devname), "/sys/usr", 64 - strlen(boot_devname));
        mntfs_mount(sys_usr, "usrfs", usrfs_get_root(), 0);
    }

    // Add standard VFS root aliases.
    extern int vfs_mount(vfs_node_t *host, const char *name, vfs_node_t *target);
    vfs_mount(vfs_root, "bin",  binfs_get_root());
    vfs_mount(vfs_root, "lib",  libfs_get_root());
    vfs_mount(vfs_root, "usr",  usrfs_get_root());
    vfs_mount(vfs_root, "dev",  devfs_get_root());
    vfs_mount(vfs_root, "proc", procfs_get_root());
    vfs_mount(vfs_root, "tmp",  tmpfs_get_root());
    vfs_mount(vfs_root, "etc",  etcfs_get_root());
    vfs_mount(vfs_root, "var",  varfs_get_root());
    vfs_mount(vfs_root, "sbin", sbinfs_get_root());
    pr_info("  %-11s : root mounted with /bin /dev /etc /proc /var\n", "mntfs");
}
