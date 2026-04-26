#include "tmpfs.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"

#define TMPFS_NAME_LEN  128
#define TMPFS_INIT_CAP  256

typedef struct tmpfs_node tmpfs_node_t;
struct tmpfs_node {
    char           name[TMPFS_NAME_LEN];
    uint32_t       type;
    char          *data;
    uint32_t       size;
    uint32_t       cap;
    uint32_t       inode;
    vfs_node_t     vnode;
    tmpfs_node_t  *children;
    tmpfs_node_t  *next;
    /*
     * Unix-style unlink semantics: when a node is unlinked while still held
     * by an open fd, it's detached from the directory tree and marked
     * pending_free.  The final close (_tmp_close) actually frees it.
     */
    int            pending_free;
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

static vfs_ops_t tmpfs_ops = {
    .read    = _tmp_read,
    .write   = _tmp_write,
    .open    = _tmp_open,
    .close   = _tmp_close,
    .walk    = _tmp_walk,
    .readdir = _tmp_readdir,
    .listdir = _tmp_listdir,
    .create  = _tmp_create,
    .delete  = _tmp_delete,
    .mkdir   = _tmp_mkdir,
    .rmdir   = _tmp_rmdir,
    .rename  = _tmp_rename,
};


static tmpfs_node_t *_find_child(tmpfs_node_t *dir, const char *name) {
    for (tmpfs_node_t *c = dir->children; c; c = c->next)
        if (streq(c->name, name)) return c;
    return 0;
}

static void _init_vnode(tmpfs_node_t *n) {
    strlcpy(n->vnode.name, n->name, 128);
    n->vnode.type     = n->type;
    n->vnode.size     = n->size;
    n->vnode.inode    = n->inode;
    n->vnode.mode     = 0777;
    n->vnode.refcount = 1;            /* 1 = link from the directory tree */
    n->vnode.ops      = &tmpfs_ops;
    n->vnode.priv     = n;
}

static void _free_tmpfs_node(tmpfs_node_t *n) {
    if (!n) return;
    /* never free the static root */
    if (n == &tmpfs_root_node) return;
    if (n->data) kfree_heap(n->data);
    kfree_heap(n);
}

static int _ensure_cap(tmpfs_node_t *n, uint32_t needed) {
    if (needed <= n->cap) return 0;
    uint32_t nc = n->cap ? n->cap * 2 : TMPFS_INIT_CAP;
    while (nc < needed) nc *= 2;
    char *nd = (char*)kmalloc(nc);
    if (!nd) return -1;
    memset(nd, 0, nc);
    if (n->data && n->size) memcpy(nd, n->data, n->size);
    if (n->data) kfree_heap(n->data);
    n->data = nd;
    n->cap  = nc;
    return 0;
}


static int _tmp_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    if (off >= n->size) return 0;
    uint32_t avail = n->size - off;
    if (size > avail) size = avail;
    memcpy(buf, n->data + off, size);
    return (int)size;
}

static int _tmp_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    uint32_t end = off + size;
    if (_ensure_cap(n, end) < 0) return -1;
    memcpy(n->data + off, buf, size);
    if (end > n->size) { n->size = end; node->size = end; }
    return (int)size;
}

/*
 * open/close: maintain vfs_node_t.refcount so _tmp_delete can defer the
 * actual free until the last fd closes (Unix-style unlink-while-open).
 * Without these, unlink() freed tmpfs_node_t (and its embedded vnode)
 * while other tasks still held pointers — classic UAF that corrupted
 * the heap once the block was reallocated.
 */
static void _tmp_open(vfs_node_t *node) {
    if (!node) return;
    node->refcount++;
}

static void _tmp_close(vfs_node_t *node) {
    if (!node || node->refcount == 0) return;
    node->refcount--;
    if (node->refcount == 0) {
        tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
        if (n && n->pending_free) _free_tmpfs_node(n);
    }
}

static vfs_node_t *_tmp_walk(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return 0;
    tmpfs_node_t *c = _find_child(d, name);
    return c ? &c->vnode : 0;
}

static vfs_dirent_t _tmp_de;
static vfs_dirent_t *_tmp_readdir(vfs_node_t *dir, uint32_t index) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return 0;
    uint32_t i = 0;
    for (tmpfs_node_t *c = d->children; c; c = c->next) {
        if (i++ == index) {
            strlcpy(_tmp_de.name, c->name, 128);
            _tmp_de.inode = c->inode;
            return &_tmp_de;
        }
    }
    return 0;
}

static void _tmp_listdir(vfs_node_t *dir) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return;
    for (tmpfs_node_t *c = d->children; c; c = c->next) {
        kprint("  "); kprint(c->name);
        kprint(c->type == VFS_DIRECTORY ? "/\n" : "\n");
    }
}

static int _tmp_create(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    if (_find_child(d, name)) return -1;
    tmpfs_node_t *n = (tmpfs_node_t*)kmalloc(sizeof(tmpfs_node_t));
    if (!n) return -1;
    memset(n, 0, sizeof(tmpfs_node_t));
    strlcpy(n->name, name, TMPFS_NAME_LEN);
    n->type  = VFS_FILE;
    n->inode = tmpfs_inode_ctr++;
    _init_vnode(n);
    n->next     = d->children;
    d->children = n;
    return 0;
}

static int _tmp_delete(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t **pp = &d->children;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            tmpfs_node_t *dead = *pp;
            *pp = dead->next;                    /* detach from directory */
            dead->next = 0;
            /*
             * The directory link itself counts as 1 in vnode.refcount (set
             * by _init_vnode).  Drop it; if any fd still holds the node,
             * defer the actual free to _tmp_close via pending_free.
             */
            if (dead->vnode.refcount > 0) dead->vnode.refcount--;
            if (dead->vnode.refcount == 0) {
                _free_tmpfs_node(dead);
            } else {
                dead->pending_free = 1;
            }
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

static int _tmp_mkdir(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    if (_find_child(d, name)) return -1;
    tmpfs_node_t *n = (tmpfs_node_t*)kmalloc(sizeof(tmpfs_node_t));
    if (!n) return -1;
    memset(n, 0, sizeof(tmpfs_node_t));
    strlcpy(n->name, name, TMPFS_NAME_LEN);
    n->type  = VFS_DIRECTORY;
    n->inode = tmpfs_inode_ctr++;
    _init_vnode(n);
    n->next     = d->children;
    d->children = n;
    return 0;
}

static int _tmp_rmdir(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t *c = _find_child(d, name);
    if (!c || c->type != VFS_DIRECTORY || c->children) return -1;
    tmpfs_node_t **pp = &d->children;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            c->next = 0;
            if (c->vnode.refcount > 0) c->vnode.refcount--;
            if (c->vnode.refcount == 0) {
                _free_tmpfs_node(c);
            } else {
                c->pending_free = 1;
            }
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

static int _tmp_rename(vfs_node_t *dir, const char *oldname, const char *newname) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t *c = _find_child(d, oldname);
    if (!c) return -1;
    strlcpy(c->name, newname, TMPFS_NAME_LEN);
    strlcpy(c->vnode.name, newname, 128);
    return 0;
}


//Public api
void tmpfs_init(void) {
    if (tmpfs_ready) return;
    memset(&tmpfs_root_node, 0, sizeof(tmpfs_node_t));
    strlcpy(tmpfs_root_node.name, "tmp", TMPFS_NAME_LEN);
    tmpfs_root_node.type  = VFS_DIRECTORY;
    tmpfs_root_node.inode = tmpfs_inode_ctr++;
    _init_vnode(&tmpfs_root_node);
    tmpfs_ready = 1;
}

vfs_node_t *tmpfs_get_root(void) {
    return &tmpfs_root_node.vnode;
}