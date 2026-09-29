#include "sbinfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "cctkfs_tree.h"

// sbinfs serves /usr/sbin: the on-disk ext4 /usr/sbin directory overlaid with a
// synthesized tree of every "/usr/sbin/..." entry in the boot archive (the
// privileged / network helpers).  ext4 wins on a name clash.  /sbin is a
// symlink into /usr/sbin.
static vfs_node_t    sbinfs_root;
static vfs_node_t   *ext4_root   = 0;
static int           sbinfs_ready = 0;
static uint32_t      sbinfs_disk_count;
static cctkfs_tree_t sbin_tree;

// Resolve ext4 /usr/sbin lazily (usrmerge: admin tools live under /usr).
static vfs_node_t *_sbin_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    vfs_node_t *usr = ext4_root->ops->walk(ext4_root, "usr");
    if (!usr || !usr->ops || !usr->ops->walk) return 0;
    return usr->ops->walk(usr, "sbin");
}

static int _on_disk(const char *name) {
    vfs_node_t *sd = _sbin_dir();
    if (sd && sd->ops && sd->ops->walk && sd->ops->walk(sd, name))
        return 1;
    return 0;
}

static void sbinfs_count_disk(void) {
    vfs_node_t *sd = _sbin_dir();
    sbinfs_disk_count = 0;
    if (!sd || !sd->ops || !sd->ops->readdir) return;
    while (sd->ops->readdir(sd, sbinfs_disk_count))
        sbinfs_disk_count++;
}

// Resolve entry from ext4 first, then the synthesized tree.
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (sd && sd->ops && sd->ops->walk) {
        vfs_node_t *disk = sd->ops->walk(sd, name);
        if (disk) return disk;
    }
    return cctkfs_tree_walk(&sbin_tree, name);
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (sd && sd->ops && sd->ops->readdir) {
        vfs_dirent_t *e = sd->ops->readdir(sd, index);
        if (e) return e;
    }
    uint32_t j = index - sbinfs_disk_count;
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&sbin_tree, &it); n;
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
    vfs_node_t *sd = _sbin_dir();
    vfs_dirent_t *de;
    int any = 0;

    if (sd && sd->ops && sd->ops->readdir) {
        for (uint32_t i = 0; (de = sd->ops->readdir(sd, i)); i++) {
            if (de->name[0] == '.') continue;
            printk("  "); printk(de->name); printk("\n");
            any = 1;
        }
    }
    cctkfs_iter_t it;
    for (vfs_node_t *n = cctkfs_tree_first(&sbin_tree, &it); n;
         n = cctkfs_iter_next(&it)) {
        if (_on_disk(n->name)) continue;
        printk("  "); printk(n->name);
        printk(n->type == VFS_DIRECTORY ? "/" : "  [cctkfs]");
        printk("\n");
        any = 1;
    }
    if (!any) printk("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /usr/sbin
static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->create) return -1;
    return sd->ops->create(sd, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->delete) return -1;
    return sd->ops->delete(sd, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->mkdir) return -1;
    return sd->ops->mkdir(sd, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->rmdir) return -1;
    return sd->ops->rmdir(sd, name);
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

vfs_node_t *sbinfs_get_root(void) { return &sbinfs_root; }

void sbinfs_init(vfs_node_t *ext4_node) {
    if (sbinfs_ready) return;

    ext4_root = ext4_node;

    memset(&sbinfs_root, 0, sizeof(vfs_node_t));
    strlcpy(sbinfs_root.name, "sbin", 128);
    sbinfs_root.type = VFS_DIRECTORY;
    sbinfs_root.mode = 0755;
    sbinfs_root.ops  = &root_ops;

    // Ensure /usr/sbin exists on ext4
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        vfs_node_t *usr = ext4_root->ops->walk(ext4_root, "usr");
        if (!usr) {
            if (ext4_root->ops->mkdir) ext4_root->ops->mkdir(ext4_root, "usr");
            usr = ext4_root->ops->walk(ext4_root, "usr");
        }
        if (usr && usr->ops && usr->ops->walk && usr->ops->mkdir &&
            !usr->ops->walk(usr, "sbin"))
            usr->ops->mkdir(usr, "sbin");
    }

    cctkfs_tree_build(&sbin_tree, "/usr/sbin/", 0755, 0);
    sbinfs_count_disk();

    uint32_t files = 0, dirs = 0;
    cctkfs_tree_count(&sbin_tree, &files, &dirs);

    pr_info("  %-11s : root ready (%u on disk, %u cctkfs files, %u dirs)\n",
            "sbinfs", sbinfs_disk_count, files, dirs);

    sbinfs_ready = 1;
}
