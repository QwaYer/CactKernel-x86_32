#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "pipe.h"
#include "blkdev.h"
#include "pci_driver.h"
#include "mouse.h"
#include "fb.h"
#include "validate.h"

// Global devfs state
static vfs_node_t    devfs_root;
static devfs_entry_t *dev_list   = 0;    // singly-linked list of registered devices
static int            devfs_ready = 0;

// /dev/modinfo — virtual file, PCI driver list (see pci_driver_modinfo_read)
static vfs_node_t modinfo_node;

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

// ctl node ops (write-only control channel)
static int _ctl_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    devfs_entry_t *e = (devfs_entry_t *)node->priv;
    if (!e || !e->drv || !e->drv->ctl) return -1;
    return e->drv->ctl(e->drv_priv, buf, size);
}

static vfs_ops_t ctl_ops = {
    .write = _ctl_write,
};

// status node ops (read-only diagnostic text)
static int _status_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    devfs_entry_t *e = (devfs_entry_t *)node->priv;
    if (!e || !e->drv || !e->drv->status) return 0;
    if (off > 0) return 0;
    char tmp[256];
    int n = e->drv->status(e->drv_priv, tmp, sizeof(tmp));
    if (n <= 0) return 0;
    if ((uint32_t)n > size) n = (int)size;
    memcpy(buf, tmp, (uint32_t)n);
    return n;
}

static vfs_ops_t status_ops = {
    .read = _status_read,
};

// per-device directory ops (data, ctl, status sub-nodes)
static vfs_node_t *_dev_dir_walk(vfs_node_t *dir, const char *name) {
    devfs_entry_t *e = (devfs_entry_t *)dir->priv;
    if (!e) return 0;
    if (streq(name, "data"))   return &e->data_node;
    if (streq(name, "ctl")   && e->drv && e->drv->ctl)    return &e->ctl_node;
    if (streq(name, "status") && e->drv && e->drv->status) return &e->status_node;
    return 0;
}

static vfs_dirent_t _dev_dir_de;

static vfs_dirent_t *_dev_dir_readdir(vfs_node_t *dir, uint32_t index) {
    devfs_entry_t *e = (devfs_entry_t *)dir->priv;
    if (!e) return 0;

    uint32_t i = 0;

    if (i++ == index) {
        strlcpy(_dev_dir_de.name, "data", 128);
        _dev_dir_de.inode = 0;
        return &_dev_dir_de;
    }
    if (e->drv && e->drv->ctl) {
        if (i++ == index) {
            strlcpy(_dev_dir_de.name, "ctl", 128);
            _dev_dir_de.inode = 1;
            return &_dev_dir_de;
        }
    }
    if (e->drv && e->drv->status) {
        if (i++ == index) {
            strlcpy(_dev_dir_de.name, "status", 128);
            _dev_dir_de.inode = 2;
            return &_dev_dir_de;
        }
    }
    return 0;
}

static void _dev_dir_listdir(vfs_node_t *dir) {
    devfs_entry_t *e = (devfs_entry_t *)dir->priv;
    if (!e) return;
    printk("  data\n");
    if (e->drv && e->drv->ctl)    printk("  ctl\n");
    if (e->drv && e->drv->status) printk("  status\n");
}

static vfs_ops_t dev_dir_ops = {
    .walk    = _dev_dir_walk,
    .readdir = _dev_dir_readdir,
    .listdir = _dev_dir_listdir,
};

// devfs root directory ops
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "modinfo")) return &modinfo_node;
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (streq(e->name, name))
            return &e->dir_node;
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
    return 0;
}

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    printk("  modinfo\n");
    for (devfs_entry_t *e = dev_list; e; e = e->next) {
        printk("  ");
        printk(e->name);
        if (!(e->flags & DEVFS_F_SIMPLE)) printk("/");
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

// populate a devfs_entry_t with its VFS nodes (simple or directory-based)
static void _fill_entry(devfs_entry_t *e) {
    memset(&e->dir_node, 0, sizeof(vfs_node_t));
    strlcpy(e->dir_node.name, e->name, 128);
    e->dir_node.priv = e;

    if (e->flags & DEVFS_F_SIMPLE) {
        e->dir_node.type = (e->flags & DEVFS_F_BLOCK)
                           ? VFS_BLOCKDEVICE : VFS_CHARDEVICE;
        e->dir_node.ops  = &data_ops;
        return;  // simple devices expose only the data node
    }

    // complex devices have a directory with data/ctl/status children
    e->dir_node.type = VFS_DIRECTORY;
    e->dir_node.ops  = &dev_dir_ops;

    memset(&e->data_node, 0, sizeof(vfs_node_t));
    strlcpy(e->data_node.name, "data", 128);
    e->data_node.type = (e->flags & DEVFS_F_BLOCK)
                        ? VFS_BLOCKDEVICE : VFS_CHARDEVICE;
    e->data_node.ops  = &data_ops;
    e->data_node.priv = e;

    memset(&e->ctl_node, 0, sizeof(vfs_node_t));
    strlcpy(e->ctl_node.name, "ctl", 128);
    e->ctl_node.type = VFS_FILE;
    e->ctl_node.ops  = &ctl_ops;
    e->ctl_node.priv = e;

    memset(&e->status_node, 0, sizeof(vfs_node_t));
    strlcpy(e->status_node.name, "status", 128);
    e->status_node.type = VFS_FILE;
    e->status_node.ops  = &status_ops;
    e->status_node.priv = e;
}

// return the devfs root node (registered in VFS mount table)
vfs_node_t *devfs_get_root(void) { return &devfs_root; }

// find a device by name in the global list
devfs_entry_t *devfs_find(const char *name) {
    for (devfs_entry_t *e = dev_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
}

// register a new device in devfs; returns the entry or NULL on duplicate/allocation failure
devfs_entry_t *register_chrdev(const char *name, uint32_t flags,
                               devfs_driver_t *drv, void *drv_priv) {
    if (!name || !drv) return 0;
    if (devfs_find(name)) {
        printk("[devfs] already registered: "); printk((char*)name); printk("\n");
        return 0;
    }

    devfs_entry_t *e = (devfs_entry_t *)kmalloc(sizeof(devfs_entry_t));
    if (!e) { printk("[devfs] kmalloc failed\n"); return 0; }
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

// one-time initialisation: set up root node and register built-in devices
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

    register_chrdev("null",    DEVFS_F_SIMPLE|DEVFS_F_CHAR,  &drv_null,   0);
    register_chrdev("zero",    DEVFS_F_SIMPLE|DEVFS_F_CHAR,  &drv_zero,   0);
    register_chrdev("random",  DEVFS_F_SIMPLE|DEVFS_F_CHAR,  &drv_random, 0);
    register_chrdev("urandom", DEVFS_F_SIMPLE|DEVFS_F_CHAR,  &drv_random, 0);

    blkdev_t *boot = blkdev_get_boot();
    if (boot) {
        register_chrdev(boot->name, DEVFS_F_BLOCK, &drv_disk, 0);
    }
    register_chrdev("tty", DEVFS_F_SIMPLE|DEVFS_F_CHAR,  &drv_tty, 0);

    register_chrdev("keyboard", DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_keyboard, 0);
    register_chrdev("mouse",    DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_mouse,    0);

    register_chrdev("fb0", DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_fb, 0);

    // Kernel-service devices (new VFS-node model)
    register_chrdev("console", DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_console, 0);
    register_chrdev("sys",     DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_sys,     0);
    register_chrdev("net",     DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_net,     0);
    register_chrdev("pipe",    DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_pipe,    0);
    register_chrdev("kmsg",    DEVFS_F_SIMPLE|DEVFS_F_CHAR, &drv_kmsg,    0);

    devfs_ready = 1;
}
