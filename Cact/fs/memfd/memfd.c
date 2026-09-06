#include "memfd.h"
#include "task.h"
#include "klib.h"

// memfd.c — VFS node glue for anonymous RAM-backed files.
//
// A memfd is created by rust_mm (memfd_create), which returns an integer
// handle.  We wrap that handle in a vfs_node (type VFS_FILE) whose ops
// dispatch read/write/truncate/stat to the Rust object, so normal file
// descriptors (and fd-level ioctls such as FSTAT / FTRUNCATE) just work.
//
// The handle is stored in node->priv.  Lifetime:
//   * memfd_create_vnode() allocates the node (refcount 0).
//   * ops->open (file_alloc/fork) increments node->refcount and takes a Rust
//     fd reference; ops->close releases it and frees the node when the last
//     reference goes away.  MAP_SHARED mmap regions hold their own Rust map
//     reference (region->shobj), so the object survives fd close while mapped.

static int _memfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _memfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _memfd_open(vfs_node_t *node);
static void _memfd_close(vfs_node_t *node);
static int _memfd_truncate(vfs_node_t *node, uint32_t length);
static int _memfd_stat(vfs_node_t *node, uint32_t *buf);

static vfs_ops_t memfd_ops = {
    .read     = _memfd_read,
    .write    = _memfd_write,
    .open     = _memfd_open,
    .close    = _memfd_close,
    .truncate = _memfd_truncate,
    .stat     = _memfd_stat,
};

static inline int _node_handle(vfs_node_t *node) {
    if (!node || node->ops != &memfd_ops) return -1;
    return (int)(intptr_t)node->priv;
}

int memfd_node_handle(vfs_node_t *node) {
    return _node_handle(node);
}

int memfd_fd_handle(int fd) {
    if (!current_task || !current_task->proc || !current_task->proc->fds) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    file_t *f = current_task->proc->fds->files[fd];
    if (!f || !f->node) return -1;
    return _node_handle(f->node);
}

static int _memfd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    int h = _node_handle(node);
    if (h < 0) return -1;
    int r = memfd_read(h, off, buf, size);
    if (r >= 0) {
        int sz = memfd_size(h);
        if (sz >= 0) node->size = (uint32_t)sz;
    }
    return r;
}

static int _memfd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    int h = _node_handle(node);
    if (h < 0) return -1;
    int r = memfd_write(h, off, buf, size);
    if (r >= 0) {
        int sz = memfd_size(h);
        if (sz >= 0) node->size = (uint32_t)sz;
    }
    return r;
}

static void _memfd_open(vfs_node_t *node) {
    int h = _node_handle(node);
    if (h < 0) return;
    node->refcount++;
    memfd_ref(h);
}

static void _memfd_close(vfs_node_t *node) {
    int h = _node_handle(node);
    if (h < 0) {
        if (node->refcount > 0) node->refcount--;
        if (node->refcount == 0) kfree(node);
        return;
    }
    if (node->refcount > 0) node->refcount--;
    if (node->refcount == 0) {
        memfd_close(h);
        node->priv = NULL;
        kfree(node);
    }
}

static int _memfd_truncate(vfs_node_t *node, uint32_t length) {
    int h = _node_handle(node);
    if (h < 0) return -1;
    if (memfd_truncate(h, length) != 0) return -1;
    node->size = length;
    return 0;
}

static int _memfd_stat(vfs_node_t *node, uint32_t *buf) {
    int h = _node_handle(node);
    if (h < 0) return -1;
    int sz = memfd_size(h);
    if (sz >= 0) node->size = (uint32_t)sz;
    vfs_fill_stat(node, buf);
    return 0;
}

vfs_node_t *memfd_create_vnode(const char *name, int flags) {
    if (!name) name = "memfd";

    const char *n = name;
    uint32_t nlen = 0;
    while (n[nlen] && nlen < 127) nlen++;

    int h = memfd_create(name, nlen, flags);
    if (h < 0) return 0;

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        memfd_close(h);
        return 0;
    }
    memset(node, 0, sizeof(vfs_node_t));

    strlcpy(node->name, name, 128);
    node->type     = VFS_FILE;
    node->size     = 0;
    node->inode    = (uint32_t)h;
    node->refcount = 0;
    node->ops      = &memfd_ops;
    node->priv     = (void *)(intptr_t)h;
    return node;
}
