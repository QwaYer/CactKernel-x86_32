#include "mntfs.h"
#include "mntfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "blkdev.h"

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
        printk("  "); printk(de->name); printk("\n");
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
vfs_ops_t raw_ops = {
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
        printk("  "); printk((char*)rest); printk("/   ["); printk(e->source); printk("]\n");
    }
}
vfs_ops_t sys_ops = {
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
    if (d->has_sys) printk("  sys/   [virtual]\n");
    printk("  raw/   [ext4]\n");
}
vfs_ops_t disk_ops = {
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
        printk("  /"); printk(d->devname);
        printk(d->has_sys ? "/   [sys+raw]\n" : "/   [raw]\n");
        any=1;
    }
    if (!any) printk("  (no disks)\n");
}
vfs_ops_t root_ops = {
    .walk=_root_walk,.readdir=_root_readdir,.listdir=_root_listdir,
};
