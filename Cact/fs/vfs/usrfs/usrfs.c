#include "usrfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "cctkfs_tree.h"

// usrfs serves /usr: the on-disk ext4 /usr directory overlaid with a
// synthesized tree of the archive's "/usr/..." entries.  /usr/bin, /usr/sbin
// and /usr/lib are owned by their own overlays (separate mounts on this node),
// so they are left out here; /usr/include and /usr/share and any nested data
// below them are synthesized as real directories.  ext4 wins on a name clash.
static vfs_node_t    usrfs_root;
static vfs_node_t   *ext4_root   = 0;
static int           usrfs_ready = 0;
static uint32_t      usrfs_disk_count;
static cctkfs_tree_t usr_tree;

// Names mounted on the usrfs root by the other overlays (see mntfs_init).
static const char *const usr_exclude[] = { "bin", "sbin", "lib", 0 };

static vfs_node_t *_usr_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "usr");
}

static int _on_disk(const char *name) {
    vfs_node_t *usr = _usr_dir();
    if (usr && usr->ops && usr->ops->walk && usr->ops->walk(usr, name))
        return 1;
    return 0;
}

static void usrfs_count_disk(void) {
    vfs_node_t *usr = _usr_dir();
    usrfs_disk_count = 0;
    if (!usr || !usr->ops || !usr->ops->readdir) return;
    while (usr->ops->readdir(usr, usrfs_disk_count))
        usrfs_disk_count++;
}

static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (usr && usr->ops && usr->ops->walk) {
        vfs_node_t *disk = usr->ops->walk(usr, name);
        if (disk) return disk;
    }
    return cctkfs_tree_walk(&usr_tree, name);
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (usr && usr->ops && usr->ops->readdir) {
        vfs_dirent_t *e = usr->ops->readdir(usr, index);
        if (e) return e;
    }
    uint32_t j = index - usrfs_disk_count;
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&usr_tree, &it); n;
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
    vfs_node_t *usr = _usr_dir();
    vfs_dirent_t *de;
    int any = 0;

    if (usr && usr->ops && usr->ops->readdir) {
        for (uint32_t i = 0; (de = usr->ops->readdir(usr, i)); i++) {
            if (de->name[0] == '.') continue;
            printk("  "); printk(de->name); printk("\n");
            any = 1;
        }
    }
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&usr_tree, &it); n;
         n = cctkfs_iter_next(&it)) {
        if (_on_disk(n->name)) continue;
        printk("  "); printk(n->name);
        printk(n->type == VFS_DIRECTORY ? "/" : "  [cctkfs]");
        printk("\n");
        any = 1;
    }
    if (!any) printk("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /usr
static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->create) return -1;
    return usr->ops->create(usr, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->delete) return -1;
    return usr->ops->delete(usr, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->mkdir) return -1;
    return usr->ops->mkdir(usr, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->rmdir) return -1;
    return usr->ops->rmdir(usr, name);
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

vfs_node_t *usrfs_get_root(void) { return &usrfs_root; }

void usrfs_init(vfs_node_t *ext4_node) {
    if (usrfs_ready) return;

    ext4_root = ext4_node;

    memset(&usrfs_root, 0, sizeof(vfs_node_t));
    strlcpy(usrfs_root.name, "usr", 128);
    usrfs_root.type = VFS_DIRECTORY;
    usrfs_root.mode = 0755;
    usrfs_root.ops  = &root_ops;

    // Ensure /usr exists on ext4
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "usr")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "usr");
        }
    }

    cctkfs_tree_build(&usr_tree, "/usr/", 0644, usr_exclude);
    usrfs_count_disk();

    uint32_t files = 0, dirs = 0;
    cctkfs_tree_count(&usr_tree, &files, &dirs);

    pr_info("  %-11s : root ready (%u on disk, %u cctkfs files, %u dirs)\n",
            "usrfs", usrfs_disk_count, files, dirs);

    usrfs_ready = 1;
}
