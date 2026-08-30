#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include "vfs.h"

// driver operations table — each method is optional
typedef struct devfs_driver {
    int  (*read)  (void *drv_priv, uint32_t off, uint32_t size, char *buf);
    int  (*write) (void *drv_priv, uint32_t off, uint32_t size, char *buf);
    int  (*ctl)   (void *drv_priv, const char *cmd, uint32_t len);
    int  (*status)(void *drv_priv, char *buf, uint32_t size);
    int  (*ioctl) (void *drv_priv, uint32_t cmd, void *arg);
} devfs_driver_t;

// entry flags
#define DEVFS_F_SIMPLE   0x01   // expose only the data node (no ctl/status subdir)
#define DEVFS_F_BLOCK    0x02   // block device
#define DEVFS_F_CHAR     0x04   // character device

typedef struct devfs_entry devfs_entry_t;

struct devfs_entry {
    char             name[64];
    uint32_t         flags;
    devfs_driver_t  *drv;
    void            *drv_priv;

    // VFS nodes — one per device
    vfs_node_t       dir_node;      // either the device itself or a directory
    vfs_node_t       data_node;     // read/write/ioctl
    vfs_node_t       ctl_node;      // control channel (write-only)
    vfs_node_t       status_node;   // diagnostic text (read-only)

    devfs_entry_t   *next;          // global linked list
};

// initialise devfs and register built-in devices (null, zero, random, tty)
void           devfs_init     (void);

// return the devfs root VFS node (to be mounted)
vfs_node_t    *devfs_get_root (void);

// register/unregister a device driver by name
devfs_entry_t *register_chrdev  (const char *name, uint32_t flags,
                                 devfs_driver_t *drv, void *drv_priv);
int            unregister_chrdev(const char *name);

// find a device entry by name (returns NULL if not found)
devfs_entry_t *devfs_find      (const char *name);

#endif