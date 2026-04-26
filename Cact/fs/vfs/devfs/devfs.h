#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include "vfs.h"

typedef struct devfs_driver {
    int  (*read)  (void *drv_priv, uint32_t off, uint32_t size, char *buf);
    int  (*write) (void *drv_priv, uint32_t off, uint32_t size, char *buf);
    int  (*ctl)   (void *drv_priv, const char *cmd, uint32_t len);
    int  (*status)(void *drv_priv, char *buf, uint32_t size);
    int  (*ioctl) (void *drv_priv, uint32_t cmd, void *arg);
} devfs_driver_t;

#define DEVFS_F_SIMPLE   0x01   
#define DEVFS_F_BLOCK    0x02   
#define DEVFS_F_CHAR     0x04  

typedef struct devfs_entry devfs_entry_t;

struct devfs_entry {
    char             name[64];
    uint32_t         flags;
    devfs_driver_t  *drv;
    void            *drv_priv;

    vfs_node_t       dir_node;
    vfs_node_t       data_node;
    vfs_node_t       ctl_node;
    vfs_node_t       status_node;

    devfs_entry_t   *next;          
};


//Public api
void           devfs_init     (void);
vfs_node_t    *devfs_get_root (void);

devfs_entry_t *devfs_register  (const char *name, uint32_t flags,
                                 devfs_driver_t *drv, void *drv_priv);
int            devfs_unregister(const char *name);
devfs_entry_t *devfs_find      (const char *name);

#endif 