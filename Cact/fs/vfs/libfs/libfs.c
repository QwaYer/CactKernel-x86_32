#include "libfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "cctkfs_tree.h"

// libfs serves /usr/lib: the on-disk ext4 /usr/lib directory overlaid with a
// synthesized tree of every "/usr/lib/..." entry in the boot archive.  That
// covers the shared libraries, /usr/lib/modules/*.cctk, /usr/lib/firmware/*
// and the nested cactpkg / cact-install payloads.  ext4 wins on a name clash.
static vfs_node_t    libfs_root;
static vfs_node_t   *ext4_root   = 0;
static int           libfs_ready = 0;
static uint32_t      libfs_disk_count;
static cctkfs_tree_t lib_tree;

// Resolve ext4 /usr/lib lazily (usrmerge: shared libraries live under /usr).
static vfs_node_t *_lib_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    vfs_node_t *usr = ext4_root->ops->walk(ext4_root, "usr");
    if (!usr || !usr->ops || !usr->ops->walk) return 0;
    return usr->ops->walk(usr, "lib");
}

static int _on_disk(const char *name) {
    vfs_node_t *lib = _lib_dir();
    if (lib && lib->ops && lib->ops->walk && lib->ops->walk(lib, name))
        return 1;
    return 0;
}

static void libfs_count_disk(void) {
    vfs_node_t *lib = _lib_dir();
    libfs_disk_count = 0;
    if (!lib || !lib->ops || !lib->ops->readdir) return;
    while (lib->ops->readdir(lib, libfs_disk_count))
        libfs_disk_count++;
}

// Resolve entry from ext4 first, then the synthesized tree.
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (lib && lib->ops && lib->ops->walk) {
        vfs_node_t *disk = lib->ops->walk(lib, name);
        if (disk) return disk;
    }
    return cctkfs_tree_walk(&lib_tree, name);
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (lib && lib->ops && lib->ops->readdir) {
        vfs_dirent_t *e = lib->ops->readdir(lib, index);
        if (e) return e;
    }
    uint32_t j = index - libfs_disk_count;
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&lib_tree, &it); n;
         n = cctkfs_iter_next(&it)) {
        if (_on_disk(n->name)) continue;
        if (j == 0) {
            strlcpy(sup_de.name, n->name, 128);
            sup_de.inode = 0;
            return &sup_de;
        }
        j--;
    }
    return 0;
}

// List ext4 /usr/lib + the cctkfs overlay with empty fallback.
static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    vfs_dirent_t *de;
    int any = 0;

    if (lib && lib->ops && lib->ops->readdir) {
        for (uint32_t i = 0; (de = lib->ops->readdir(lib, i)); i++) {
            if (de->name[0] == '.') continue;
            printk("  "); printk(de->name); printk("\n");
            any = 1;
        }
    }
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&lib_tree, &it); n;
         n = cctkfs_iter_next(&it)) {
        if (_on_disk(n->name)) continue;
        printk("  "); printk(n->name);
        printk(n->type == VFS_DIRECTORY ? "/" : "  [cctkfs]");
        printk("\n");
        any = 1;
    }
    if (!any) printk("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /usr/lib
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

// Initialize libfs and ensure /usr/lib exists.
void libfs_init(vfs_node_t *ext4_node) {
    if (libfs_ready) return;

    ext4_root = ext4_node;

    memset(&libfs_root, 0, sizeof(vfs_node_t));
    strlcpy(libfs_root.name, "lib", 128);
    libfs_root.type = VFS_DIRECTORY;
    libfs_root.mode = 0755;
    libfs_root.ops  = &root_ops;

    // Ensure /usr/lib exists on ext4
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        vfs_node_t *usr = ext4_root->ops->walk(ext4_root, "usr");
        if (!usr) {
            if (ext4_root->ops->mkdir) ext4_root->ops->mkdir(ext4_root, "usr");
            usr = ext4_root->ops->walk(ext4_root, "usr");
        }
        if (usr && usr->ops && usr->ops->walk && usr->ops->mkdir &&
            !usr->ops->walk(usr, "lib"))
            usr->ops->mkdir(usr, "lib");
    }

    cctkfs_tree_build(&lib_tree, "/usr/lib/", 0755, 0);
    libfs_count_disk();

    uint32_t files = 0, dirs = 0;
    cctkfs_tree_count(&lib_tree, &files, &dirs);

    pr_info("  %-11s : root ready (%u on disk, %u cctkfs files, %u dirs)\n",
            "libfs", libfs_disk_count, files, dirs);

    libfs_ready = 1;
}
