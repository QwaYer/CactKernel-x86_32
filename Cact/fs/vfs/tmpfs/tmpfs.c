#include "tmpfs.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"

// tmpfs: RAM-backed namespace (directories + names).  File *contents* live in
// the generic VFS inode address space (see vfs_as_*), a memfd-backed page
// cache shared with MAP_SHARED mmap — so read()/write() and a mapping observe
// one storage.  tmpfs owns only the directory tree: names on dirents (hard
// links work), inodes freed when links + open handles reach zero.

#define TMPFS_NAME_LEN  128

typedef struct tmpfs_node   tmpfs_node_t;
typedef struct tmpfs_dirent tmpfs_dirent_t;

struct tmpfs_dirent {
    char            name[TMPFS_NAME_LEN];
    tmpfs_node_t   *inode;
    tmpfs_dirent_t *next;
};

struct tmpfs_node {
    uint32_t        type;       // VFS_FILE or VFS_DIRECTORY
    uint32_t        inode;      // inode number
    uint32_t        nlink;      // number of dirents pointing here
    vfs_node_t      vnode;      // embedded VFS node (the inode handle)
    tmpfs_dirent_t *children;   // directory entries (only for VFS_DIRECTORY)
};

static tmpfs_node_t  tmpfs_root_node;
static uint32_t      tmpfs_inode_ctr = 1;
static int           tmpfs_ready     = 0;

static int _tmp_read   (vfs_node_t*, uint32_t, uint32_t, char*);
static int _tmp_write  (vfs_node_t*, uint32_t, uint32_t, char*);
static void _tmp_open  (vfs_node_t*);
static void _tmp_close (vfs_node_t*);
static vfs_node_t *_tmp_walk   (vfs_node_t*, const char*);
static vfs_dirent_t *_tmp_readdir(vfs_node_t*, uint32_t);
static void _tmp_listdir(vfs_node_t*);
static int _tmp_create (vfs_node_t*, const char*);
static int _tmp_delete (vfs_node_t*, const char*);
static int _tmp_mkdir  (vfs_node_t*, const char*);
static int _tmp_rmdir  (vfs_node_t*, const char*);
static int _tmp_rename (vfs_node_t*, const char*, const char*);
static int _tmp_link   (vfs_node_t*, const char*, vfs_node_t*);
static int _tmp_truncate(vfs_node_t*, uint32_t);
static int _tmp_mmap_backing(vfs_node_t*, uint32_t, uint32_t, int*, uint32_t*);

static vfs_ops_t tmpfs_ops = {
    .read         = _tmp_read,
    .write        = _tmp_write,
    .open         = _tmp_open,
    .close        = _tmp_close,
    .walk         = _tmp_walk,
    .readdir      = _tmp_readdir,
    .listdir      = _tmp_listdir,
    .create       = _tmp_create,
    .delete       = _tmp_delete,
    .mkdir        = _tmp_mkdir,
    .rmdir        = _tmp_rmdir,
    .rename       = _tmp_rename,
    .link         = _tmp_link,
    .mmap_backing = _tmp_mmap_backing,
    .truncate     = _tmp_truncate,
};

static tmpfs_dirent_t *_find_dirent(tmpfs_node_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY) return 0;
    for (tmpfs_dirent_t *d = dir->children; d; d = d->next)
        if (streq(d->name, name)) return d;
    return 0;
}

static tmpfs_node_t *_find_child(tmpfs_node_t *dir, const char *name) {
    tmpfs_dirent_t *d = _find_dirent(dir, name);
    return d ? d->inode : 0;
}

static void _init_vnode(tmpfs_node_t *n, const char *name) {
    int i = 0;
    if (name)
        while (name[i] && i < 127) { n->vnode.name[i] = name[i]; i++; }
    n->vnode.name[i] = '\0';
    n->vnode.type     = n->type;
    n->vnode.size     = 0;
    n->vnode.inode    = n->inode;
    n->vnode.mode     = 0777;
    n->vnode.refcount = 0;            // _add_dirent gives it its first link ref
    n->vnode.ops      = &tmpfs_ops;
    n->vnode.priv     = n;
}

static void _free_inode(tmpfs_node_t *n) {
    if (!n || n == &tmpfs_root_node) return;
    vfs_as_release(n->vnode.priv, n->vnode.inode);   // drop the page-cache object
    kfree(n);
}

// File I/O goes through the generic inode address space (memfd page cache),
// keyed by (filesystem instance, inode number).
static int _tmp_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    vfs_as_setsize(node->priv, node->inode, node->size);
    return vfs_as_read(node->priv, node->inode, off, size, buf);
}

static int _tmp_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    int w = vfs_as_write(node->priv, node->inode, off, size, buf);
    if (w > 0) {
        int s = vfs_as_size(node->priv, node->inode);
        if (s >= 0) node->size = (uint32_t)s;
    }
    return w;
}

static int _tmp_truncate(vfs_node_t *node, uint32_t length) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    int r = vfs_as_truncate(node->priv, node->inode, length);
    if (r == 0) node->size = length;
    return r;
}

static int _tmp_mmap_backing(vfs_node_t *node, uint32_t off, uint32_t len,
                             int *backing, uint32_t *obj_off) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    vfs_as_setsize(node->priv, node->inode, node->size);
    return vfs_as_backing(node->priv, node->inode, off, len, backing, obj_off);
}

static void _tmp_open(vfs_node_t *node) {
    if (!node) return;
    __sync_fetch_and_add(&node->refcount, 1);
}

static void _tmp_close(vfs_node_t *node) {
    if (!node || node->refcount == 0) return;
    if (__sync_fetch_and_sub(&node->refcount, 1) == 1) {
        tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
        _free_inode(n);
    }
}

static vfs_node_t *_tmp_walk(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    tmpfs_node_t *c = _find_child(d, name);
    return c ? &c->vnode : 0;
}

static vfs_dirent_t _tmp_de;

static vfs_dirent_t *_tmp_readdir(vfs_node_t *dir, uint32_t index) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return 0;
    uint32_t i = 0;
    for (tmpfs_dirent_t *e = d->children; e; e = e->next) {
        if (i++ == index) {
            strlcpy(_tmp_de.name, e->name, 128);
            _tmp_de.inode = e->inode->inode;
            return &_tmp_de;
        }
    }
    return 0;
}

static void _tmp_listdir(vfs_node_t *dir) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return;
    for (tmpfs_dirent_t *e = d->children; e; e = e->next) {
        printk("  "); printk(e->name);
        printk(e->inode->type == VFS_DIRECTORY ? "/\n" : "\n");
    }
}

static int _add_dirent(tmpfs_node_t *dir, const char *name, tmpfs_node_t *inode) {
    if (_find_dirent(dir, name)) return -1;
    tmpfs_dirent_t *e = (tmpfs_dirent_t*)kmalloc(sizeof(tmpfs_dirent_t));
    if (!e) return -1;
    memset(e, 0, sizeof(tmpfs_dirent_t));
    strlcpy(e->name, name, TMPFS_NAME_LEN);
    e->inode = inode;
    e->next  = dir->children;
    dir->children = e;
    inode->nlink++;
    __sync_fetch_and_add(&inode->vnode.refcount, 1);
    return 0;
}

static tmpfs_node_t *_alloc_inode(uint32_t type) {
    tmpfs_node_t *n = (tmpfs_node_t*)kmalloc(sizeof(tmpfs_node_t));
    if (!n) return 0;
    memset(n, 0, sizeof(tmpfs_node_t));
    n->type  = type;
    n->inode = tmpfs_inode_ctr++;
    return n;
}

static int _tmp_create(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t *n = _alloc_inode(VFS_FILE);
    if (!n) return -1;
    _init_vnode(n, name);
    if (_add_dirent(d, name, n) != 0) { _free_inode(n); return -1; }
    return 0;
}

static int _tmp_delete(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_dirent_t **pp = &d->children;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            tmpfs_dirent_t *dead = *pp;
            tmpfs_node_t   *ino  = dead->inode;
            *pp = dead->next;
            kfree(dead);
            if (ino->nlink) ino->nlink--;
            if (__sync_fetch_and_sub(&ino->vnode.refcount, 1) == 1)
                _free_inode(ino);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

static int _tmp_mkdir(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t *n = _alloc_inode(VFS_DIRECTORY);
    if (!n) return -1;
    _init_vnode(n, name);
    if (_add_dirent(d, name, n) != 0) { _free_inode(n); return -1; }
    return 0;
}

static int _tmp_rmdir(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_dirent_t *e = _find_dirent(d, name);
    if (!e || e->inode->type != VFS_DIRECTORY || e->inode->children) return -1;
    return _tmp_delete(dir, name);
}

static int _tmp_rename(vfs_node_t *dir, const char *oldname, const char *newname) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_dirent_t *e = _find_dirent(d, oldname);
    if (!e) return -1;
    if (_find_dirent(d, newname)) return -1;
    strlcpy(e->name, newname, TMPFS_NAME_LEN);
    if (e->inode->nlink <= 1)
        strlcpy(e->inode->vnode.name, newname, 128);
    return 0;
}

static int _tmp_link(vfs_node_t *dir, const char *name, vfs_node_t *target_node) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY || !target_node) return -1;
    tmpfs_node_t *ino = (tmpfs_node_t*)target_node->priv;
    if (!ino || ino->type == VFS_DIRECTORY) return -1;
    return _add_dirent(d, name, ino);
}

static void _root_init(tmpfs_node_t *n, const char *name) {
    memset(n, 0, sizeof(tmpfs_node_t));
    n->type  = VFS_DIRECTORY;
    n->inode = tmpfs_inode_ctr++;
    _init_vnode(n, name);
    n->vnode.refcount = 1;
}

vfs_node_t *tmpfs_create_root(const char *name) {
    tmpfs_node_t *n = (tmpfs_node_t *)kmalloc(sizeof(tmpfs_node_t));
    if (!n) return 0;
    _root_init(n, name ? name : "rootfs");
    return &n->vnode;
}

void tmpfs_init(void) {
    if (tmpfs_ready) return;
    _root_init(&tmpfs_root_node, "tmp");
    // Superblock profile for statfs (RAM filesystem: block size 4096).
    vfs_sb_register("tmpfs", 4096, 0, 0, 0, 0, 0);
    pr_info("  %-11s : root ready\n", "tmpfs");
    tmpfs_ready = 1;
}

vfs_node_t *tmpfs_get_root(void) {
    return &tmpfs_root_node.vnode;
}
