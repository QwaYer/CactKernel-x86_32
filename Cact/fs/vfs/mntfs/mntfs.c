#include "mntfs.h"
#include "etcfs.h"
#include "devfs.h"
#include "procfs.h"
#include "tmpfs.h"
#include "binfs.h"
#include "sbinfs.h"
#include "libfs.h"
#include "varfs.h"
#include "vfs.h"
#include "ext4.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "blkdev.h"

// Global linked lists
static mntfs_entry_t  *mnt_list   = 0;    // virtual mount points (sys/...)
static disk_entry_t   *disk_list  = 0;    // physical disks (raw + sys)
static vfs_node_t      mntfs_root;
static int             mntfs_ready = 0;
static char            boot_devname[32];   // name of the boot block device

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

    extern int vfs_mount(vfs_node_t *host, const char *name, vfs_node_t *target);
    vfs_mount(vfs_root, "bin",  binfs_get_root());
    vfs_mount(vfs_root, "sbin", sbinfs_get_root());
    vfs_mount(vfs_root, "lib",  libfs_get_root());
    vfs_mount(vfs_root, "dev",  devfs_get_root());
    vfs_mount(vfs_root, "proc", procfs_get_root());
    vfs_mount(vfs_root, "tmp",  tmpfs_get_root());
    vfs_mount(vfs_root, "etc",  etcfs_get_root());
    vfs_mount(vfs_root, "var",  varfs_get_root());
}

// Find a mount point by full path
static mntfs_entry_t *_find(const char *name) {
    for (mntfs_entry_t *e = mnt_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
}

// Find a disk entry by device name
static disk_entry_t *_find_disk(const char *devname) {
    for (disk_entry_t *d = disk_list; d; d = d->next)
        if (streq(d->devname, devname)) return d;
    return 0;
}

// Build per-disk sys prefix ("<disk>/sys/").
static int _sys_pfx(disk_entry_t *d, char *pfx) {
    strlcpy(pfx, d->devname, MNTFS_NAME_LEN);
    int l = 0; while (pfx[l]) l++;
    pfx[l++]='/'; pfx[l++]='s'; pfx[l++]='y'; pfx[l++]='s'; pfx[l++]='/';
    pfx[l] = '\0';
    return l;
}

// Append device name to /etc/mounts.
static void _mounts_add(const char *devname) {
    char cur[1024]; memset(cur, 0, sizeof(cur));
    int len = etcfs_read("mounts", cur, sizeof(cur) - 64);
    if (len < 0) len = 0;
    int dlen = 0; while (devname[dlen]) dlen++;
    for (int i = 0; i <= len - dlen; i++) {
        int m = 1;
        for (int j = 0; j < dlen; j++) if (cur[i+j] != devname[j]) { m=0; break; }
        if (m && (cur[i+dlen]=='\n'||cur[i+dlen]=='\0')) return; // already there
    }
    int p = len;
    for (int i = 0; devname[i] && p < (int)sizeof(cur)-2; i++) cur[p++] = devname[i];
    cur[p++]='\n'; cur[p]='\0';
    etcfs_write("mounts", cur, (uint32_t)p);
}

// Remove device name from /etc/mounts.
static void _mounts_remove(const char *devname) {
    char cur[1024]; memset(cur, 0, sizeof(cur));
    int len = etcfs_read("mounts", cur, sizeof(cur) - 1);
    if (len <= 0) return;
    cur[len] = '\0';
    char out[1024]; int oi=0, i=0;
    int dlen=0; while (devname[dlen]) dlen++;
    while (i < len) {
        int start=i;
        while (i<len && cur[i]!='\n') i++;
        int end=i; if (i<len) i++;
        int llen=end-start;
        int match=(llen==dlen);
        if (match) for (int k=0;k<llen;k++) if (cur[start+k]!=devname[k]) { match=0; break; }
        if (!match) {
            for (int k=start; k<end && oi<(int)sizeof(out)-2; k++) out[oi++]=cur[k];
            if (oi<(int)sizeof(out)-1) out[oi++]='\n';
        }
    }
    out[oi]='\0';
    etcfs_write("mounts", out, (uint32_t)oi);
}

// Mount devices listed in /etc/mounts.
static void _mounts_mount_all(void) {
    char buf[1024]; memset(buf, 0, sizeof(buf));
    int len = etcfs_read("mounts", buf, sizeof(buf) - 1);
    if (len <= 0) { kprint("[mounts] empty\n"); return; }
    buf[len] = '\0';
    int i = 0;
    while (i < len) {
        char line[32]; int li=0;
        while (i<len && buf[i]!='\n' && li<31) line[li++]=buf[i++];
        line[li]='\0'; if (i<len) i++;
        while (li>0 && (line[li-1]==' '||line[li-1]=='\r')) line[--li]='\0';
        if (li==0 || line[0]=='#') continue;
        if (_find_disk(line)) continue; // already mounted
        uint32_t dev;
        if (mntfs_resolve_device(line, &dev) < 0) {
            kprint("[mounts] unknown: "); kprint(line); kprint("\n"); continue;
        }
        vfs_node_t *node = ext4_mount_disk(dev);
        if (!node) { kprint("[mounts] failed: "); kprint(line); kprint("\n"); continue; }
        mntfs_mount_disk(line, node, 0);
        kprint("[mounts] "); kprint(line); kprint(" mounted\n");
    }
}

// raw/ subdirectory ops: pass-through to ext4 root
static vfs_node_t *_raw_walk(vfs_node_t *dir, const char *n) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    return (d&&d->ext4_root&&d->ext4_root->ops->walk)
           ? d->ext4_root->ops->walk(d->ext4_root,n) : 0;
}
static vfs_dirent_t *_raw_readdir(vfs_node_t *dir, uint32_t idx) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    return (d&&d->ext4_root&&d->ext4_root->ops->readdir)
           ? d->ext4_root->ops->readdir(d->ext4_root,idx) : 0;
}
static void _raw_listdir(vfs_node_t *dir) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d||!d->ext4_root||!d->ext4_root->ops->readdir) return;
    vfs_dirent_t *de;
    for (uint32_t i=0;(de=d->ext4_root->ops->readdir(d->ext4_root,i));i++) {
        if (de->name[0]=='.' && (de->name[1]=='\0'||(de->name[1]=='.'&&de->name[2]=='\0'))) continue;
        kprint("  "); kprint(de->name); kprint("\n");
    }
}
static int _raw_create(vfs_node_t *dir, const char *n) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    return (d&&d->ext4_root&&d->ext4_root->ops->create)
           ? d->ext4_root->ops->create(d->ext4_root,n) : -1;
}
static int _raw_delete(vfs_node_t *dir, const char *n) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    return (d&&d->ext4_root&&d->ext4_root->ops->delete)
           ? d->ext4_root->ops->delete(d->ext4_root,n) : -1;
}
static int _raw_mkdir(vfs_node_t *dir, const char *n) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    return (d&&d->ext4_root&&d->ext4_root->ops->mkdir)
           ? d->ext4_root->ops->mkdir(d->ext4_root,n) : -1;
}
static int _raw_rmdir(vfs_node_t *dir, const char *n) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    return (d&&d->ext4_root&&d->ext4_root->ops->rmdir)
           ? d->ext4_root->ops->rmdir(d->ext4_root,n) : -1;
}
static vfs_ops_t raw_ops = {
    .walk=_raw_walk,.readdir=_raw_readdir,.listdir=_raw_listdir,
    .create=_raw_create,.delete=_raw_delete,.mkdir=_raw_mkdir,.rmdir=_raw_rmdir,
};

// sys/ subdirectory ops: virtual mount points under this disk
static vfs_node_t *_sys_walk(vfs_node_t *dir, const char *name) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d) return 0;
    char pfx[MNTFS_NAME_LEN]; int pl=_sys_pfx(d,pfx);
    char full[MNTFS_NAME_LEN]; strlcpy(full,pfx,MNTFS_NAME_LEN);
    int fl=pl;
    for (int i=0;name[i]&&fl<MNTFS_NAME_LEN-1;i++) full[fl++]=name[i];
    full[fl]='\0';
    mntfs_entry_t *e=_find(full);
    return e ? e->target : 0;
}
static vfs_dirent_t _sys_de;
static vfs_dirent_t *_sys_readdir(vfs_node_t *dir, uint32_t index) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d) return 0;
    char pfx[MNTFS_NAME_LEN]; int pl=_sys_pfx(d,pfx);
    uint32_t found=0;
    for (mntfs_entry_t *e=mnt_list;e;e=e->next) {
        int m=1; for (int k=0;k<pl;k++) if (e->name[k]!=pfx[k]) {m=0;break;}
        if (!m) continue;
        const char *rest=e->name+pl;
        int has_slash=0; for (int k=0;rest[k];k++) if (rest[k]=='/') {has_slash=1;break;}
        if (has_slash) continue;
        if (found++==index) { strlcpy(_sys_de.name,rest,128); _sys_de.inode=found; return &_sys_de; }
    }
    return 0;
}
static void _sys_listdir(vfs_node_t *dir) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d) return;
    char pfx[MNTFS_NAME_LEN]; int pl=_sys_pfx(d,pfx);
    for (mntfs_entry_t *e=mnt_list;e;e=e->next) {
        int m=1; for (int k=0;k<pl;k++) if (e->name[k]!=pfx[k]) {m=0;break;}
        if (!m) continue;
        const char *rest=e->name+pl;
        int has_slash=0; for (int k=0;rest[k];k++) if (rest[k]=='/') {has_slash=1;break;}
        if (has_slash) continue;
        kprint("  "); kprint((char*)rest); kprint("/   ["); kprint(e->source); kprint("]\n");
    }
}
static vfs_ops_t sys_ops = {
    .walk=_sys_walk,.readdir=_sys_readdir,.listdir=_sys_listdir,
};

// Per-disk directory ops (sys/ and raw/ children)
static vfs_node_t *_disk_walk(vfs_node_t *dir, const char *name) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d) return 0;
    if (streq(name,"raw")) return &d->raw_node;
    if (streq(name,"sys") && d->has_sys) return &d->sys_node;
    return 0;
}
static vfs_dirent_t _disk_de;
static vfs_dirent_t *_disk_readdir(vfs_node_t *dir, uint32_t index) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d) return 0;
    uint32_t i=0;
    if (d->has_sys) {
        if (i++==index) { strlcpy(_disk_de.name,"sys",128); _disk_de.inode=1; return &_disk_de; }
    }
    if (i++==index) { strlcpy(_disk_de.name,"raw",128); _disk_de.inode=2; return &_disk_de; }
    return 0;
}
static void _disk_listdir(vfs_node_t *dir) {
    disk_entry_t *d=(disk_entry_t*)dir->priv;
    if (!d) return;
    if (d->has_sys) kprint("  sys/   [virtual]\n");
    kprint("  raw/   [ext4]\n");
}
static vfs_ops_t disk_ops = {
    .walk=_disk_walk,.readdir=_disk_readdir,.listdir=_disk_listdir,
};

// mntfs root directory ops: lists all disks
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    disk_entry_t *d=_find_disk(name);
    return d ? &d->disk_node : 0;
}
static vfs_dirent_t _root_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    uint32_t i=0;
    for (disk_entry_t *d=disk_list;d;d=d->next)
        if (i++==index) { strlcpy(_root_de.name,d->devname,128); _root_de.inode=i; return &_root_de; }
    return 0;
}
static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    int any=0;
    for (disk_entry_t *d=disk_list;d;d=d->next) {
        kprint("  /"); kprint(d->devname);
        kprint(d->has_sys ? "/   [sys+raw]\n" : "/   [raw]\n");
        any=1;
    }
    if (!any) kprint("  (no disks)\n");
}
static vfs_ops_t root_ops = {
    .walk=_root_walk,.readdir=_root_readdir,.listdir=_root_listdir,
};

// Resolve a device name to a block device ID (uses blkdev_find)
int mntfs_resolve_device(const char *devname, uint32_t *dev_out) {
    blkdev_t *bd = blkdev_find(devname);
    if (bd) {
        if (dev_out) *dev_out = 0;   // single-device system, dev=0
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
int mntfs_mount_disk(const char *devname, vfs_node_t *ext4_root, int persistent) {
    if (!devname||!ext4_root) return -1;
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
            kfree_heap(dead); return 0;
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
            kfree_heap(dead); return 0;
        }
        pp=&(*pp)->next;
    }
    return -1;
}

// Print all mounted disks and virtual mount points
void mntfs_list(void) {
    kprint("\nDisks:\n");
    int any=0;
    for (disk_entry_t *d=disk_list;d;d=d->next) {
        kprint("  /"); kprint(d->devname);
        kprint(d->has_sys ? "  [master: sys+raw]" : "  [slave: raw]");
        kprint(d->persistent ? "  [saved]\n" : "\n");
        any=1;
    }
    if (!any) kprint("  (none)\n");
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
        kprint("[mntfs] WARNING: no boot block device (load ahci/nvme kmod for disk)\n");
        klog(LOG_WARN, "mntfs: no boot disk — nodisk mode");
        mntfs_setup_nodisk();
        klog(LOG_OK, "mntfs: virtual root ready (no boot disk)");
        return;
    }

    strlcpy(boot_devname, boot->name, 32);
    vfs_node_t *ext4 = ext4_mount_disk(0);
    if (!ext4) {
        kprint("[mntfs] WARNING: no ext4 on "); kprint(boot_devname); kprint("\n");
        klog(LOG_WARN, "mntfs: ext4 mount failed — nodisk mode");
        mntfs_setup_nodisk();
        klog(LOG_OK, "mntfs: virtual root ready (ext4 unavailable)");
        return;
    }
    mntfs_mount_disk(boot_devname, ext4, 0);
    disk_entry_t *boot_disk = _find_disk(boot_devname);
    boot_disk->has_sys = 1;   // master disk gets sys/ virtual directory

    // Mount subsystem roots under <boot>/sys.
    etcfs_init(ext4);
    char sys_etc[64];
    strlcpy(sys_etc, boot_devname, 64);
    strlcpy(sys_etc + strlen(boot_devname), "/sys/etc", 64 - strlen(boot_devname));
    mntfs_mount(sys_etc, "etcfs", etcfs_get_root(), 0);

    _mounts_mount_all();

    devfs_init();
    char sys_dev[64];
    strlcpy(sys_dev, boot_devname, 64);
    strlcpy(sys_dev + strlen(boot_devname), "/sys/dev", 64 - strlen(boot_devname));
    mntfs_mount(sys_dev, "devfs", devfs_get_root(), 0);

    procfs_init();
    char sys_proc[64];
    strlcpy(sys_proc, boot_devname, 64);
    strlcpy(sys_proc + strlen(boot_devname), "/sys/proc", 64 - strlen(boot_devname));
    mntfs_mount(sys_proc, "procfs", procfs_get_root(), 0);

    tmpfs_init();
    char sys_tmp[64];
    strlcpy(sys_tmp, boot_devname, 64);
    strlcpy(sys_tmp + strlen(boot_devname), "/sys/tmp", 64 - strlen(boot_devname));
    mntfs_mount(sys_tmp, "tmpfs", tmpfs_get_root(), 0);

    binfs_init(ext4);
    char sys_bin[64];
    strlcpy(sys_bin, boot_devname, 64);
    strlcpy(sys_bin + strlen(boot_devname), "/sys/bin", 64 - strlen(boot_devname));
    mntfs_mount(sys_bin, "binfs", binfs_get_root(), 0);

    sbinfs_init(ext4);
    char sys_sbin[64];
    strlcpy(sys_sbin, boot_devname, 64);
    strlcpy(sys_sbin + strlen(boot_devname), "/sys/sbin", 64 - strlen(boot_devname));
    mntfs_mount(sys_sbin, "sbinfs", sbinfs_get_root(), 0);

    libfs_init(ext4);
    char sys_lib[64];
    strlcpy(sys_lib, boot_devname, 64);
    strlcpy(sys_lib + strlen(boot_devname), "/sys/lib", 64 - strlen(boot_devname));
    mntfs_mount(sys_lib, "libfs", libfs_get_root(), 0);

    varfs_init(ext4);
    char sys_var[64];
    strlcpy(sys_var, boot_devname, 64);
    strlcpy(sys_var + strlen(boot_devname), "/sys/var", 64 - strlen(boot_devname));
    mntfs_mount(sys_var, "varfs", varfs_get_root(), 0);

    // Add standard VFS root aliases.
    extern int vfs_mount(vfs_node_t *host, const char *name, vfs_node_t *target);
    vfs_mount(vfs_root, "bin",  binfs_get_root());
    vfs_mount(vfs_root, "lib",  libfs_get_root());
    vfs_mount(vfs_root, "dev",  devfs_get_root());
    vfs_mount(vfs_root, "proc", procfs_get_root());
    vfs_mount(vfs_root, "tmp",  tmpfs_get_root());
    vfs_mount(vfs_root, "etc",  etcfs_get_root());
    vfs_mount(vfs_root, "var",  varfs_get_root());
    vfs_mount(vfs_root, "sbin", sbinfs_get_root());
    klog(LOG_OK, "mntfs: root filesystem and /bin /dev /proc mounts ready");
}