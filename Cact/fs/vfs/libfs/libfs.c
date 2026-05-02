#include "libfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

// libfs root node — all ops forward to ext4's /lib directory
static vfs_node_t  libfs_root;
static vfs_node_t *ext4_root   = 0;
static int         libfs_ready = 0;

// Helper: resolve ext4's /lib directory (lazy, on first use)
static vfs_node_t *_lib_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "lib");
}

// Forward walk to ext4 /lib/<name>
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->walk) return 0;
    return lib->ops->walk(lib, name);
}

// Forward readdir to ext4 /lib
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->readdir) return 0;
    return lib->ops->readdir(lib, index);
}

// Forward listdir to ext4 /lib (with fallback for empty directory)
static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->readdir) {
        kprint("  (empty)\n");
        return;
    }
    vfs_dirent_t *de;
    int any = 0;
    for (uint32_t i = 0; (de = lib->ops->readdir(lib, i)); i++) {
        if (de->name[0] == '.') continue;
        kprint("  "); kprint(de->name); kprint("\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /lib
static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->create) return -1;
    return lib->ops->create(lib, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->delete) return -1;
    return lib->ops->delete(lib, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->mkdir) return -1;
    return lib->ops->mkdir(lib, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->rmdir) return -1;
    return lib->ops->rmdir(lib, name);
}

// VFS ops table for libfs root
static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
    .create  = _root_create,
    .delete  = _root_delete,
    .mkdir   = _root_mkdir,
    .rmdir   = _root_rmdir,
};

// Return the libfs root node (registered in VFS mount table)
vfs_node_t *libfs_get_root(void) { return &libfs_root; }

// Initialise libfs: bind to ext4's /lib, create directory if missing
void libfs_init(vfs_node_t *ext4_node) {
    if (libfs_ready) return;

    ext4_root = ext4_node;

    memset(&libfs_root, 0, sizeof(vfs_node_t));
    strlcpy(libfs_root.name, "lib", 128);
    libfs_root.type = VFS_DIRECTORY;
    libfs_root.mode = 0755;
    libfs_root.ops  = &root_ops;

    // Ensure /lib exists on ext4
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "lib")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "lib");
        }
    }

    libfs_ready = 1;
    klog(LOG_OK, "libfs ready — /lib available");
}