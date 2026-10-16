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

// devfs root directory ops
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "modinfo")) return &modinfo_node;
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (streq(e->name, name))
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

    if (e->flags & DEVFS_F_DIR) {
        e->node.type = VFS_DIRECTORY;
        e->node.ops  = &dev_subdir_ops;
        return;
    }

    e->node.type = (e->flags & DEVFS_F_BLOCK) ? VFS_BLOCKDEVICE : VFS_CHARDEVICE;
    e->node.ops  = &data_ops;
}

// return the devfs root node (registered in VFS mount table)
vfs_node_t *devfs_get_root(void) { return &devfs_root; }

// find a device by name in the global list
devfs_entry_t *devfs_find(const char *name) {
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
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

// register a new device in devfs; returns the entry or NULL on duplicate/allocation failure
devfs_entry_t *register_chrdev(const char *name, uint32_t flags,
                               devfs_driver_t *drv, void *drv_priv) {
    if (!name || !drv) return 0;
    if (devfs_find(name)) {
        pr_warn("[devfs] already registered: %s\n", name);
        return 0;
    }

    devfs_entry_t *e = (devfs_entry_t *)kmalloc(sizeof(devfs_entry_t));
    if (!e) { pr_err("[devfs] kmalloc failed\n"); return 0; }
    memset(e, 0, sizeof(devfs_entry_t));

    strlcpy(e->name, name, 64);
    e->flags    = flags;
    e->drv      = drv;
    e->drv_priv = drv_priv;
    _fill_entry(e);

    e->next  = dev_list;
    dev_list = e;
    return e;
}

// remove a device from devfs by name; returns 0 on success, -1 if not found
int unregister_chrdev(const char *name) {
    devfs_entry_t **pp = &dev_list;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            devfs_entry_t *dead = *pp;
            *pp = dead->next;
            kfree(dead);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
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
    for (devfs_entry_t *e = dev_list; e; e = e->next) ndev++;

    pr_info("  %-11s : root ready (%u device node(s) + %d proc node(s) + /dev/modinfo)\n",
            "devfs", ndev, extra_count);

    devfs_ready = 1;
}
