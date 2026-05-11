#include "varfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

// varfs root forwards operations to ext4 /var.
static vfs_node_t  varfs_root;
static vfs_node_t *ext4_root   = 0;
static int         varfs_ready = 0;

// Resolve ext4 /var lazily.
static vfs_node_t *_var_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "var");
}

// Forward walk to ext4 /var/<name>
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->walk) return 0;
    return var->ops->walk(var, name);
}

static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->readdir) return 0;
    return var->ops->readdir(var, index);
}

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->readdir) {
        kprint("  (empty)\n");
        return;
    }
    vfs_dirent_t *de;
    int any = 0;
    for (uint32_t i = 0; (de = var->ops->readdir(var, i)); i++) {
        if (de->name[0] == '.') continue;
        kprint("  "); kprint(de->name); kprint("\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
}

static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->create) return -1;
    return var->ops->create(var, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->delete) return -1;
    return var->ops->delete(var, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->mkdir) return -1;
    return var->ops->mkdir(var, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *var = _var_dir();
    if (!var || !var->ops || !var->ops->rmdir) return -1;
    return var->ops->rmdir(var, name);
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

vfs_node_t *varfs_get_root(void) { return &varfs_root; }

void varfs_init(vfs_node_t *ext4_node) {
    if (varfs_ready) return;

    ext4_root = ext4_node;

    memset(&varfs_root, 0, sizeof(vfs_node_t));
    strlcpy(varfs_root.name, "var", 128);
    varfs_root.type = VFS_DIRECTORY;
    varfs_root.mode = 0755;
    varfs_root.ops  = &root_ops;

    // Ensure /var and /var/log exist on ext4 so userspace services can drop
    // their state files without first having to mkdir the system hierarchy.
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "var")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "var");
        }
        vfs_node_t *var = ext4_root->ops->walk(ext4_root, "var");
        if (var && var->ops && var->ops->walk && var->ops->mkdir) {
            if (!var->ops->walk(var, "log"))
                var->ops->mkdir(var, "log");
        }
    }

    varfs_ready = 1;
}
