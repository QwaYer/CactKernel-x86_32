#include "binfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "cctkfs_tree.h"

// binfs serves /usr/bin: the on-disk ext4 /usr/bin directory overlaid with a
// synthesized tree of every "/usr/bin/..." entry in the boot archive (the user
// ELFs).  ext4 wins on a name clash.  /bin is a symlink into /usr/bin.
static vfs_node_t    binfs_root;
static vfs_node_t   *ext4_root   = 0;
static int           binfs_ready = 0;
static uint32_t      binfs_disk_count;
static cctkfs_tree_t bin_tree;

// Resolve ext4 /usr/bin lazily (usrmerge: user binaries live under /usr).
static vfs_node_t *_bin_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    vfs_node_t *usr = ext4_root->ops->walk(ext4_root, "usr");
    if (!usr || !usr->ops || !usr->ops->walk) return 0;
    return usr->ops->walk(usr, "bin");
}

static int _on_disk(const char *name) {
    vfs_node_t *bin = _bin_dir();
    if (bin && bin->ops && bin->ops->walk && bin->ops->walk(bin, name))
        return 1;
    return 0;
}

static void binfs_count_disk(void) {
    vfs_node_t *bin = _bin_dir();
    binfs_disk_count = 0;
    if (!bin || !bin->ops || !bin->ops->readdir) return;
    while (bin->ops->readdir(bin, binfs_disk_count))
        binfs_disk_count++;
}

// Resolve entry from ext4 first, then the synthesized tree.
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (bin && bin->ops && bin->ops->walk) {
        vfs_node_t *disk = bin->ops->walk(bin, name);
        if (disk) return disk;
    }
    return cctkfs_tree_walk(&bin_tree, name);
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (bin && bin->ops && bin->ops->readdir) {
        vfs_dirent_t *e = bin->ops->readdir(bin, index);
        if (e) return e;
    }
    uint32_t j = index - binfs_disk_count;
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&bin_tree, &it); n;
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

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    vfs_dirent_t *de;
    int any = 0;

    if (bin && bin->ops && bin->ops->readdir) {
        for (uint32_t i = 0; (de = bin->ops->readdir(bin, i)); i++) {
            if (de->name[0] == '.') continue;
            printk("  "); printk(de->name); printk("\n");
            any = 1;
        }
    }
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&bin_tree, &it); n;
         n = cctkfs_iter_next(&it)) {
        if (_on_disk(n->name)) continue;
        printk("  "); printk(n->name);
        printk(n->type == VFS_DIRECTORY ? "/" : "  [cctkfs]");
        printk("\n");
        any = 1;
    }
    if (!any) printk("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /usr/bin
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

vfs_node_t *binfs_get_root(void) { return &binfs_root; }

void binfs_init(vfs_node_t *ext4_node) {
    if (binfs_ready) return;

    ext4_root = ext4_node;

    memset(&binfs_root, 0, sizeof(vfs_node_t));
    strlcpy(binfs_root.name, "bin", 128);
    binfs_root.type = VFS_DIRECTORY;
    binfs_root.mode = 0755;
    binfs_root.ops  = &root_ops;

    // Ensure /usr/bin exists on ext4
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        vfs_node_t *usr = ext4_root->ops->walk(ext4_root, "usr");
        if (!usr) {
            if (ext4_root->ops->mkdir) ext4_root->ops->mkdir(ext4_root, "usr");
            usr = ext4_root->ops->walk(ext4_root, "usr");
        }
        if (usr && usr->ops && usr->ops->walk && usr->ops->mkdir &&
            !usr->ops->walk(usr, "bin"))
            usr->ops->mkdir(usr, "bin");
    }

    cctkfs_tree_build(&bin_tree, "/usr/bin/", 0755, 0);
    binfs_count_disk();

    uint32_t files = 0, dirs = 0;
    cctkfs_tree_count(&bin_tree, &files, &dirs);

    pr_info("  %-11s : root ready (%u on disk, %u cctkfs files, %u dirs)\n",
            "binfs", binfs_disk_count, files, dirs);

    binfs_ready = 1;
}
