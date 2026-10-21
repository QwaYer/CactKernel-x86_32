#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include "vfs.h"

// driver operations table — each method is optional
//
// read/write/ioctl/... feed the single node a device is registered as.
// `ctl` and `status` are legacy slots: devfs no longer models them as
// data/ctl/status sub-nodes (a device is one node now), but the two pointers
// stay in their original positions because out-of-tree PCI modules (NVMe,
// AHCI, HDA, ...) are compiled against this layout and still set `status`.
typedef struct devfs_driver {
    int  (*read)  (void *drv_priv, uint32_t off, uint32_t size, char *buf);
    int  (*write) (void *drv_priv, uint32_t off, uint32_t size, char *buf);
    int  (*ctl)   (void *drv_priv, const char *cmd, uint32_t len);
    int  (*status)(void *drv_priv, char *buf, uint32_t size);
    int  (*ioctl) (void *drv_priv, uint32_t cmd, void *arg);

    /* DEVFS_F_DIR entries only: resolve and enumerate the children of this
     * device's directory (e.g. /dev/dri -> card0, renderD128).  The returned
     * nodes belong to the driver, which supplies their ops table — that is how
     * a subsystem gets its own ioctl/mmap behaviour without devfs knowing
     * anything about it. */
    vfs_node_t   *(*walk)   (void *drv_priv, const char *name);
    vfs_dirent_t *(*readdir)(void *drv_priv, uint32_t index);
} devfs_driver_t;

// entry flags
#define DEVFS_F_BLOCK    0x02   // block device
#define DEVFS_F_CHAR     0x04   // character device
#define DEVFS_F_DIR      0x08   // directory whose children come from drv->walk/readdir
#define DEVFS_F_LINK     0x10   // synthetic symlink, target in link_target

typedef struct devfs_entry devfs_entry_t;

// One registered device is exactly one VFS node (Linux-style): a char device, a
// block device, a directory or a (compat) symlink.  There are no
// data/ctl/status sub-nodes.
//
// Entries form a tree: `parent` is the directory entry that owns this one
// (NULL = /dev itself), so a device can be registered inside a group directory
// ("input/keyboard" -> /dev/input/keyboard).  The fields below `next` were
// appended after the original layout, so out-of-tree modules compiled against
// the older struct keep working unchanged.
struct devfs_entry {
    char             name[64];
    uint32_t         flags;
    devfs_driver_t  *drv;
    void            *drv_priv;

    vfs_node_t       node;          // the device node itself

    devfs_entry_t   *next;          // global linked list (all entries)

    devfs_entry_t   *parent;        // owning directory entry, NULL = /dev root
    devfs_entry_t   *compat;        // root compat symlink made for a grouped node
    char             link_target[128]; // DEVFS_F_LINK: symlink target
};

// initialise devfs and register built-in devices (null, zero, random, ttyN, ...)
void           devfs_init     (void);

// return the devfs root VFS node (to be mounted)
vfs_node_t    *devfs_get_root (void);

// register/unregister a device driver by name
//
// `name` may be a path: "disk/sda" registers the node inside the group
// directory /dev/disk (created on demand) and also publishes a compat symlink
// /dev/sda -> disk/sda, so legacy flat paths keep resolving.  A plain name
// registers in the /dev root as before.
devfs_entry_t *register_chrdev  (const char *name, uint32_t flags,
                                 devfs_driver_t *drv, void *drv_priv);
int            unregister_chrdev(const char *name);

// Create (or find) a group directory under /dev, e.g. "input", "disk/by-id".
// Intermediate components are created as needed; returns the leaf entry or NULL.
devfs_entry_t *devfs_mkdir      (const char *path);

// Publish (or refresh) a synthetic symlink at `path`, which may include group
// directories ("disk/by-id/ata-Samsung_SSD_850"), pointing at `target`.  The
// target is stored verbatim: a relative target resolves from the directory
// holding the link.  Returns the entry, or NULL if the name is held by a real
// node.
devfs_entry_t *devfs_symlink    (const char *path, const char *target);

// Remove a non-directory devfs entry (device node or symlink) by path.
// Returns 0 on success, -1 when absent or when the path names a directory.
int            devfs_remove     (const char *path);

// find a device entry by name (returns NULL if not found)
devfs_entry_t *devfs_find      (const char *name);

// Publish a ready-made VFS node (with its own ops/fops) directly in the devfs
// root.  Used by the per-process nodes (/dev/tty, /dev/stdin, /dev/fd, ...)
// and by directory nodes such as /dev/pts, which cannot be expressed as a
// plain devfs_driver_t.  The node must stay alive for the kernel's lifetime.
int            devfs_add_node  (const char *name, vfs_node_t *node);

#endif
