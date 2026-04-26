#include "binfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

static vfs_node_t  binfs_root;
static vfs_node_t *ext4_root   = 0;
static int         binfs_ready = 0;

static vfs_node_t *_bin_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "bin");
}

static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->walk) return 0;
    return bin->ops->walk(bin, name);
}

static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->readdir) return 0;
    return bin->ops->readdir(bin, index);
}

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->readdir) {
        kprint("  (empty)\n");
        return;
    }
    vfs_dirent_t *de;
    int any = 0;
    for (uint32_t i = 0; (de = bin->ops->readdir(bin, i)); i++) {
        if (de->name[0] == '.') continue;
        kprint("  "); kprint(de->name); kprint("\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
}

static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->create) return -1;
    return bin->ops->create(bin, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->delete) return -1;
    return bin->ops->delete(bin, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->mkdir) return -1;
    return bin->ops->mkdir(bin, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->rmdir) return -1;
    return bin->ops->rmdir(bin, name);
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
    .create  = _root_create,
    .delete  = _root_delete,
    .mkdir   = _root_mkdir,
    .rmdir   = _root_rmdir,
};


/* Public API */
vfs_node_t *binfs_get_root(void) { return &binfs_root; }

void binfs_init(vfs_node_t *ext4_node) {
    if (binfs_ready) return;

    ext4_root = ext4_node;

    memset(&binfs_root, 0, sizeof(vfs_node_t));
    strlcpy(binfs_root.name, "bin", 128);
    binfs_root.type = VFS_DIRECTORY;
    binfs_root.mode = 0755;
    binfs_root.ops  = &root_ops;

    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "bin")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "bin");
        }
    }

    binfs_ready = 1;
    klog(LOG_OK, "binfs ready — /bin available");
}