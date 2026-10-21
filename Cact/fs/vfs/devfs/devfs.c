#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "pci_driver.h"
#include "tty.h"

// Global devfs state
static vfs_node_t    devfs_root;
static devfs_entry_t *dev_list   = 0;    // singly-linked list of registered devices
static int            devfs_ready = 0;

// /dev/modinfo — virtual file, PCI driver list (see pci_driver_modinfo_read)
static vfs_node_t modinfo_node;

// Ready-made nodes published straight into the root (per-process nodes, /dev/fd
// and /dev/pts directories, ...).  See devfs_add_node().
#define DEVFS_MAX_EXTRA 24
static struct {
    const char *name;
    vfs_node_t *node;
} extra_nodes[DEVFS_MAX_EXTRA];
static int extra_count = 0;

// data node ops (read/write/ioctl)
static int _data_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    devfs_entry_t *e = (devfs_entry_t *)node->priv;
    if (!e || !e->drv || !e->drv->read) return -1;
    return e->drv->read(e->drv_priv, off, size, buf);
}

static int _data_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    devfs_entry_t *e = (devfs_entry_t *)node->priv;
    if (!e || !e->drv || !e->drv->write) return -1;
    return e->drv->write(e->drv_priv, off, size, buf);
}

static int _data_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    devfs_entry_t *e = (devfs_entry_t *)node->priv;
    if (!e || !e->drv || !e->drv->ioctl) return -1;
    return e->drv->ioctl(e->drv_priv, cmd, arg);
}

static vfs_ops_t data_ops = {
    .read  = _data_read,
    .write = _data_write,
    .ioctl = _data_ioctl,
};

// Directory-entry ops (DEVFS_F_DIR): the registered driver owns the children,
// so walk/readdir just forward to it.  This is how e.g. /dev/dri exposes
// card0 / renderD128 nodes whose ioctl and mmap behaviour is entirely the
// driver's own.
static vfs_node_t *_subdir_walk(vfs_node_t *dir, const char *name) {
    devfs_entry_t *e = (devfs_entry_t *)dir->priv;
    if (!e || !e->drv || !e->drv->walk) return 0;
    return e->drv->walk(e->drv_priv, name);
}

static vfs_dirent_t *_subdir_readdir(vfs_node_t *dir, uint32_t index) {
    devfs_entry_t *e = (devfs_entry_t *)dir->priv;
    if (!e || !e->drv || !e->drv->readdir) return 0;
    return e->drv->readdir(e->drv_priv, index);
}

static vfs_ops_t dev_subdir_ops = {
    .walk    = _subdir_walk,
    .readdir = _subdir_readdir,
};

// Group-directory ops (DEVFS_F_DIR with no drv): the children are the entries
// whose ->parent is this directory, so a group is a view over the entry tree
// (Linux-style /dev/input, /dev/disk) rather than a driver-owned subtree.
static vfs_node_t *_group_walk(vfs_node_t *dir, const char *name) {
    devfs_entry_t *g = (devfs_entry_t *)dir->priv;
    if (!g) return 0;
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (e->parent == g && streq(e->name, name))
            return &e->node;
    return 0;
}

static vfs_dirent_t _group_de;

static vfs_dirent_t *_group_readdir(vfs_node_t *dir, uint32_t index) {
    devfs_entry_t *g = (devfs_entry_t *)dir->priv;
    if (!g) return 0;
    uint32_t i = 0;
    for (devfs_entry_t *e = dev_list; e; e = e->next) {
        if (e->parent != g) continue;
        if (i++ == index) {
            strlcpy(_group_de.name, e->name, 128);
            _group_de.inode = i;
            return &_group_de;
        }
    }
    return 0;
}

static void _group_listdir(vfs_node_t *dir) {
    devfs_entry_t *g = (devfs_entry_t *)dir->priv;
    if (!g) return;
    for (devfs_entry_t *e = dev_list; e; e = e->next) {
        if (e->parent != g) continue;
        printk("  ");
        printk(e->name);
        if (e->flags & DEVFS_F_DIR) printk("/");
        printk("\n");
    }
}

static vfs_ops_t group_ops = {
    .walk    = _group_walk,
    .readdir = _group_readdir,
    .listdir = _group_listdir,
};

// devfs root directory ops
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "modinfo")) return &modinfo_node;
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (!e->parent && streq(e->name, name))
            return &e->node;
    for (int i = 0; i < extra_count; i++)
        if (streq(extra_nodes[i].name, name))
            return extra_nodes[i].node;
    return 0;
}

static vfs_dirent_t _root_de;

static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    if (index == 0) {
        strlcpy(_root_de.name, "modinfo", 128);
        _root_de.inode = 0;
        return &_root_de;
    }
    uint32_t i = 1;
    for (devfs_entry_t *e = dev_list; e; e = e->next) {
        if (e->parent) continue;
        if (i++ == index) {
            strlcpy(_root_de.name, e->name, 128);
            _root_de.inode = i;
            return &_root_de;
        }
    }
    for (int k = 0; k < extra_count; k++) {
        if (i++ == index) {
            strlcpy(_root_de.name, extra_nodes[k].name, 128);
            _root_de.inode = i;
            return &_root_de;
        }
    }
    return 0;
}

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    printk("  modinfo\n");
    for (devfs_entry_t *e = dev_list; e; e = e->next) {
        if (e->parent) continue;
        printk("  ");
        printk(e->name);
        if (e->flags & DEVFS_F_DIR) printk("/");
        printk("\n");
    }
    for (int k = 0; k < extra_count; k++) {
        printk("  ");
        printk(extra_nodes[k].name);
        if (extra_nodes[k].node->type == VFS_DIRECTORY) printk("/");
        printk("\n");
    }
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
};

static int _modinfo_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)node;
    return pci_driver_modinfo_read(off, size, buf);
}

static vfs_ops_t modinfo_ops = { .read = _modinfo_read };

// populate a devfs_entry_t with its single VFS node
static void _fill_entry(devfs_entry_t *e) {
    memset(&e->node, 0, sizeof(vfs_node_t));
    strlcpy(e->node.name, e->name, 128);
    e->node.priv = e;

    if (e->flags & DEVFS_F_LINK) {
        // Synthetic symlink: the VFS reads the target straight out of
        // node->priv (see vfs_readlink_node), so no ops table is needed.
        e->node.type = VFS_SYMLINK;
        e->node.mode = 0777;
        e->node.size = (uint32_t)strlen(e->link_target);
        e->node.priv = e->link_target;
        return;
    }

    if (e->flags & DEVFS_F_DIR) {
        e->node.type = VFS_DIRECTORY;
        // A driver-owned directory (e.g. /dev/dri) enumerates through the
        // driver; a plain group directory enumerates its entry children.
        e->node.ops  = e->drv ? &dev_subdir_ops : &group_ops;
        return;
    }

    e->node.type = (e->flags & DEVFS_F_BLOCK) ? VFS_BLOCKDEVICE : VFS_CHARDEVICE;
    e->node.ops  = &data_ops;
}

// return the devfs root node (registered in VFS mount table)
vfs_node_t *devfs_get_root(void) { return &devfs_root; }

// find a device by (leaf) name anywhere in the tree
devfs_entry_t *devfs_find(const char *name) {
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
}

// find a child of `parent` by name (parent == 0 means the /dev root)
static devfs_entry_t *_find_in(devfs_entry_t *parent, const char *name) {
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (e->parent == parent && streq(e->name, name)) return e;
    return 0;
}

// detach `e` from the global entry list (does not free it)
static void _unlink_entry(devfs_entry_t *e) {
    devfs_entry_t **pp = &dev_list;
    while (*pp) {
        if (*pp == e) { *pp = e->next; return; }
        pp = &(*pp)->next;
    }
}

// allocate an entry, fill its node and link it under `parent`
static devfs_entry_t *_new_entry(devfs_entry_t *parent, const char *name,
                                 uint32_t flags, devfs_driver_t *drv, void *drv_priv) {
    devfs_entry_t *e = (devfs_entry_t *)kmalloc(sizeof(devfs_entry_t));
    if (!e) { pr_err("devfs: kmalloc failed allocating entry '%s'\n", name); return 0; }
    memset(e, 0, sizeof(devfs_entry_t));
    strlcpy(e->name, name, 64);
    e->flags    = flags;
    e->drv      = drv;
    e->drv_priv = drv_priv;
    e->parent   = parent;
    _fill_entry(e);
    e->next  = dev_list;
    dev_list = e;
    return e;
}

// Resolve "a/b/c" to a group entry, creating the components when `create` is set.
static devfs_entry_t *_resolve_dir(const char *path, int create) {
    devfs_entry_t *parent = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[64];
        int n = 0;
        while (*p && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = 0;

        devfs_entry_t *d = _find_in(parent, comp);
        if (!d) {
            if (!create) return 0;
            d = _new_entry(parent, comp, DEVFS_F_DIR, 0, 0);
            if (!d) return 0;
        }
        parent = d;
    }
    return parent;
}

// Create (or find) a group directory under /dev (e.g. "input", "disk/by-id").
devfs_entry_t *devfs_mkdir(const char *path) {
    if (!path || !path[0]) return 0;
    return _resolve_dir(path, 1);
}

// Split "dir/leaf" into a group directory and the leaf name.  Returns the leaf
// (a pointer into `name`); *parent_out is the owning directory (0 = /dev root).
// `create` says whether missing group components may be created.
static const char *_split_path(const char *name, devfs_entry_t **parent_out, int create) {
    const char *slash = 0;
    for (const char *p = name; *p; p++)
        if (*p == '/') slash = p;
    *parent_out = 0;
    if (slash) {
        char dir[64];
        int dl = (int)(slash - name);
        if (dl <= 0 || dl >= (int)sizeof(dir)) return 0;
        memcpy(dir, name, (uint32_t)dl);
        dir[dl] = 0;
        *parent_out = _resolve_dir(dir, create);
        if (!*parent_out) return 0;
    }
    return slash ? slash + 1 : name;
}

// register a new device in devfs; returns the entry or NULL on duplicate/allocation failure
devfs_entry_t *register_chrdev(const char *name, uint32_t flags,
                               devfs_driver_t *drv, void *drv_priv) {
    if (!name || !name[0] || !drv) return 0;

    // "group/leaf" puts the node under /dev/group; a plain name stays in root.
    devfs_entry_t *parent = 0;
    const char *leaf = _split_path(name, &parent, 1);
    if (!leaf || !leaf[0]) return 0;

    if (_find_in(parent, leaf)) {
        pr_warn("devfs: already registered: %s\n", name);
        return 0;
    }

    devfs_entry_t *e = _new_entry(parent, leaf, flags, drv, drv_priv);
    if (!e) return 0;

    // A grouped node keeps a compat symlink at the old flat name so legacy
    // paths (/dev/keyboard, /dev/sda) keep resolving.
    if (parent && !_find_in(0, leaf)) {
        devfs_entry_t *c = _new_entry(0, leaf, DEVFS_F_LINK, 0, 0);
        if (c) {
            strlcpy(c->link_target, name, sizeof(c->link_target));
            _fill_entry(c);
            e->compat = c;
        }
    }
    return e;
}

// Remove an entry by path.  `allow_dir` false refuses to remove a directory.
// A grouped node's compat symlink goes away with it.
static int _remove_path(const char *path, int allow_dir) {
    if (!path || !path[0]) return -1;

    devfs_entry_t *parent = 0;
    const char *leaf = _split_path(path, &parent, 0);   // find only
    if (!leaf || !leaf[0]) return -1;

    devfs_entry_t *dead = _find_in(parent, leaf);
    if (!dead) return -1;
    if (!allow_dir && (dead->flags & DEVFS_F_DIR)) return -1;

    devfs_entry_t *compat = dead->compat;
    _unlink_entry(dead);
    kfree(dead);
    if (compat) {
        _unlink_entry(compat);
        kfree(compat);
    }
    return 0;
}

// remove a device from devfs by name; returns 0 on success, -1 if not found
int unregister_chrdev(const char *name) {
    return _remove_path(name, 1);
}

// Remove a non-directory devfs entry (device node or symlink) by path.
int devfs_remove(const char *path) {
    return _remove_path(path, 0);
}

// Publish (or refresh) a synthetic symlink at `path`, which may include group
// directories ("disk/by-id/ata-...").  The target is stored verbatim: a
// relative target resolves from the directory that holds the link.
devfs_entry_t *devfs_symlink(const char *path, const char *target) {
    if (!path || !path[0] || !target || !target[0]) return 0;

    devfs_entry_t *parent = 0;
    const char *leaf = _split_path(path, &parent, 1);
    if (!leaf || !leaf[0]) return 0;

    devfs_entry_t *e = _find_in(parent, leaf);
    if (!e)
        e = _new_entry(parent, leaf, DEVFS_F_LINK, 0, 0);
    else if (!(e->flags & DEVFS_F_LINK))
        return 0;               // the name is held by a real node
    if (!e) return 0;

    strlcpy(e->link_target, target, sizeof(e->link_target));
    _fill_entry(e);             // the node's size/priv come from the target
    return e;
}

int devfs_add_node(const char *name, vfs_node_t *node) {
    if (!name || !node) return -1;
    if (extra_count >= DEVFS_MAX_EXTRA) return -1;
    if (devfs_find(name)) return -1;
    for (int i = 0; i < extra_count; i++)
        if (streq(extra_nodes[i].name, name)) return -1;

    extra_nodes[extra_count].name = name;
    extra_nodes[extra_count].node = node;
    extra_count++;
    return 0;
}

// One-time initialisation: set up the root node and let each device owner file
// register its own nodes (devfs keeps no device list of its own).
void devfs_init(void) {
    if (devfs_ready) return;

    memset(&devfs_root, 0, sizeof(vfs_node_t));
    strlcpy(devfs_root.name, "dev", 128);
    devfs_root.type = VFS_DIRECTORY;
    devfs_root.ops  = &root_ops;

    memset(&modinfo_node, 0, sizeof(vfs_node_t));
    strlcpy(modinfo_node.name, "modinfo", 128);
    modinfo_node.type = VFS_FILE;
    modinfo_node.ops  = &modinfo_ops;

    // Linux-convention group directories that hold no device of their own:
    // /dev/shm (shared memory) and /dev/mqueue (POSIX message queues).  They
    // exist so the usual paths resolve and a tmpfs/queue filesystem can be
    // mounted over them later.
    devfs_mkdir("shm");
    devfs_mkdir("mqueue");

    // devfs itself registers no device: each owner file registers the nodes it
    // defines (tty_alloc() for the terminals; devfs_devices_init() /
    // devfs_services_init() / devfs_crypto_init() for the rest), and block
    // devices come from vfsdev.  This keeps devfs a generic device filesystem.
    tty_init();
    devfs_devices_init();
    devfs_services_init();
    devfs_crypto_init();

    // Per-process nodes (/dev/tty, /dev/stdin|stdout|stderr, /dev/fd, /dev/core)
    // and the pty namespace (/dev/ptmx, /dev/pts).
    devfs_proc_init();

    uint32_t ndev = 0;
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (!e->parent) ndev++;

    pr_info("%s: root ready (%u node(s) in /dev + %d proc node(s) + /dev/modinfo)\n",
            "devfs", ndev, extra_count);

    devfs_ready = 1;
}
