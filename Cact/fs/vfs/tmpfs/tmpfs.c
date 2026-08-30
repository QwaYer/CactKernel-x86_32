#include "tmpfs.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"

#define TMPFS_NAME_LEN  128
#define TMPFS_INIT_CAP  256    // initial file capacity

typedef struct tmpfs_node tmpfs_node_t;
struct tmpfs_node {
    char           name[TMPFS_NAME_LEN];
    uint32_t       type;        // VFS_FILE or VFS_DIRECTORY
    char          *data;        // file contents (heap, cap-sized)
    uint32_t       size;        // current data length
    uint32_t       cap;         // allocated capacity
    uint32_t       inode;
    vfs_node_t     vnode;       // embedded VFS node (refcounted)
    tmpfs_node_t  *children;    // directory children (linked list)
    tmpfs_node_t  *next;        // sibling link
    int            pending_free;// detached from tree but still held by open fd
};

// Static root node — never freed
static tmpfs_node_t  tmpfs_root_node;
static uint32_t      tmpfs_inode_ctr = 1;
static int           tmpfs_ready     = 0;

// Forward declarations
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

// Find a child by name in a directory node
static tmpfs_node_t *_find_child(tmpfs_node_t *dir, const char *name) {
    for (tmpfs_node_t *c = dir->children; c; c = c->next)
        if (streq(c->name, name)) return c;
    return 0;
}

// Initialise the embedded VFS node from the tmpfs metadata
static void _init_vnode(tmpfs_node_t *n) {
    strlcpy(n->vnode.name, n->name, 128);
    n->vnode.type     = n->type;
    n->vnode.size     = n->size;
    n->vnode.inode    = n->inode;
    n->vnode.mode     = 0777;
    n->vnode.refcount = 1;            // 1 = directory tree link
    n->vnode.ops      = &tmpfs_ops;
    n->vnode.priv     = n;
}

// Free tmpfs node storage (except static root).
static void _free_tmpfs_node(tmpfs_node_t *n) {
    if (!n) return;
    if (n == &tmpfs_root_node) return;   // static root, never freed
    if (n->data) kfree(n->data);
    kfree(n);
}

// Ensure file capacity using doubling growth.
static int _ensure_cap(tmpfs_node_t *n, uint32_t needed) {
    if (needed <= n->cap) return 0;
    uint32_t nc = n->cap ? n->cap * 2 : TMPFS_INIT_CAP;
    while (nc < needed) nc *= 2;
    char *nd = (char*)kmalloc(nc);
    if (!nd) return -1;
    memset(nd, 0, nc);
    if (n->data && n->size) memcpy(nd, n->data, n->size);
    if (n->data) kfree(n->data);
    n->data = nd;
    n->cap  = nc;
    return 0;
}

// Read from a regular file
static int _tmp_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    if (off >= n->size) return 0;
    uint32_t avail = n->size - off;
    if (size > avail) size = avail;
    memcpy(buf, n->data + off, size);
    return (int)size;
}

// Write to a regular file (auto-expand capacity)
static int _tmp_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
    if (!n || n->type != VFS_FILE) return -1;
    uint32_t end = off + size;
    if (_ensure_cap(n, end) < 0) return -1;
    memcpy(n->data + off, buf, size);
    if (end > n->size) { n->size = end; node->size = end; }
    return (int)size;
}

// Increment vnode refcount on open.
static void _tmp_open(vfs_node_t *node) {
    if (!node) return;
    node->refcount++;
}

// Decrement vnode refcount and free pending nodes.
static void _tmp_close(vfs_node_t *node) {
    if (!node || node->refcount == 0) return;
    if (__sync_fetch_and_sub(&node->refcount, 1) == 1) {
        tmpfs_node_t *n = (tmpfs_node_t*)node->priv;
        if (n && n->pending_free) _free_tmpfs_node(n);
    }
}

// Walk a directory to find a child by name
static vfs_node_t *_tmp_walk(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return 0;
    tmpfs_node_t *c = _find_child(d, name);
    return c ? &c->vnode : 0;
}

static vfs_dirent_t _tmp_de;

// Return the directory entry at a given index
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

// Print directory listing (non-recursive)
static void _tmp_listdir(vfs_node_t *dir) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return;
    for (tmpfs_node_t *c = d->children; c; c = c->next) {
        printk("  "); printk(c->name);
        printk(c->type == VFS_DIRECTORY ? "/\n" : "\n");
    }
}

// Create an empty regular file in a directory
static int _tmp_create(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    if (_find_child(d, name)) return -1;   // duplicate

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

// Unlink file; defer free while open.
static int _tmp_delete(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t **pp = &d->children;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            tmpfs_node_t *dead = *pp;
            *pp = dead->next;                    // detach from directory
            dead->next = 0;
            // Drop tree reference; defer free if still referenced.
            if (__sync_fetch_and_sub(&dead->vnode.refcount, 1) == 1) {
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

// Create a subdirectory
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

// Remove empty subdirectory.
static int _tmp_rmdir(vfs_node_t *dir, const char *name) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t *c = _find_child(d, name);
    if (!c || c->type != VFS_DIRECTORY || c->children) return -1; // not empty
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

// Rename a file or directory within the same parent
static int _tmp_rename(vfs_node_t *dir, const char *oldname, const char *newname) {
    tmpfs_node_t *d = (tmpfs_node_t*)dir->priv;
    if (!d || d->type != VFS_DIRECTORY) return -1;
    tmpfs_node_t *c = _find_child(d, oldname);
    if (!c) return -1;
    strlcpy(c->name, newname, TMPFS_NAME_LEN);
    strlcpy(c->vnode.name, newname, 128);
    return 0;
}

// Initialize tmpfs root node.
void tmpfs_init(void) {
    if (tmpfs_ready) return;
    memset(&tmpfs_root_node, 0, sizeof(tmpfs_node_t));
    strlcpy(tmpfs_root_node.name, "tmp", TMPFS_NAME_LEN);
    tmpfs_root_node.type  = VFS_DIRECTORY;
    tmpfs_root_node.inode = tmpfs_inode_ctr++;
    _init_vnode(&tmpfs_root_node);
    tmpfs_ready = 1;
}

// Return the root VFS node (to be mounted)
vfs_node_t *tmpfs_get_root(void) {
    return &tmpfs_root_node.vnode;
}