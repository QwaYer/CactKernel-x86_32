#ifndef FS_MOD_H
#define FS_MOD_H

#include <stdint.h>
#include "vfs.h"

// Generic entry point exported by a loadable filesystem module (.cctk):
// mount block device `dev` and return the root node (or NULL on failure).
typedef vfs_node_t* (*fs_mount_fn_t)(uint32_t dev);

// Load a relocatable filesystem module (.cctk) from the staged cctkfs image
// and resolve its `fs_mount` entry point. Returns 0 on success.
int  fs_mod_load(const char *path);

// Unload the currently loaded filesystem module (no-op if none).
void fs_mod_unload(void);

// Mount block device `dev` through the loaded filesystem module.
vfs_node_t *fs_mod_mount(uint32_t dev);

// Whether a filesystem module is currently loaded.
int fs_mod_loaded(void);

// Non-destructive probe: does the module at 'path' export the generic
// filesystem entry point 'fs_mount'? Returns 1 if it is a filesystem module,
// 0 if not, and a negative error code if it cannot be read/validated.
int fs_mod_detect(const char *path);

#endif
