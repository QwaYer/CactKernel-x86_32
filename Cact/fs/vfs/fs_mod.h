#ifndef FS_MOD_H
#define FS_MOD_H

#include <stdint.h>
#include "vfs.h"
#include "blkdev.h"

// Loadable filesystem modules (.cctk).
//
// A filesystem module is a relocatable ET_REL image (typically ext4.cctk)
// staged in the cctkfs module blob. It exports:
//     vfs_node_t *fs_mount(blkdev_t *dev);   // mount, return root or NULL
//     int         fs_unmount(void);          // optional teardown
// The loader keeps up to FS_MOD_MAX modules resident at once ("multi-slot"),
// each under a short instance name derived from the module path basename
// (e.g. "ext4"). Mounting a block device either selects a module by that
// name (fs_mod_mount_type) or tries every loaded module in registration
// order until one accepts the device (fs_mod_mount).

#define FS_MOD_MAX           8
#define FS_MOD_MAX_MOUNTS    32

// Generic entry points exported by a loadable filesystem module.
typedef vfs_node_t* (*fs_mount_fn_t)(blkdev_t *dev);
typedef int         (*fs_unmount_fn_t)(void);

// Load a filesystem module from the staged cctkfs image and resolve its
// entry points. Returns 0 on success, negative error otherwise.
int fs_mod_load(const char *path);

// Unload by instance name ("ext4") or slot index. Refuses while the module
// still has mounted devices. Returns 0 on success, negative otherwise.
int fs_mod_unload(const char *instance);
int fs_mod_unload_slot(int slot);

// Query.
int          fs_mod_count(void);            // number of resident modules
int          fs_mod_loaded(const char *instance); // 1 if that module is loaded
const char  *fs_mod_instance(int slot);     // instance name or NULL
int          fs_mod_loaded_any(void);       // 1 if at least one module resident

// Mount a block device.  fs_mod_mount tries every resident module until one
// accepts the device; fs_mod_mount_type only uses the module whose instance
// name equals fstype (or falls back to autodetect for "auto"/"*").
vfs_node_t *fs_mod_mount     (blkdev_t *dev);
vfs_node_t *fs_mod_mount_type(blkdev_t *dev, const char *fstype);

// Drop a mounted device from a module slot (called on umount).
int fs_mod_unmount_dev(blkdev_t *dev);

// Non-destructive probe: does the module at 'path' export the generic
// filesystem entry point 'fs_mount'? Returns 1 if it is a filesystem module,
// 0 if not, and a negative error code if it cannot be read/validated.
int fs_mod_detect(const char *path);

#endif
