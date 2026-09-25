#ifndef VFSDEV_H
#define VFSDEV_H

#include "blkdev.h"

// vfsdev — block-device node + mount manager ("mounter").
//
// Responsibilities:
//   * register one /dev/<name> block node (VFS_BLOCKDEVICE) for every block
//     device (whole disk and partition). devfs keeps the character/service
//     nodes; both live in the same /dev tree.
//   * mount/unmount a filesystem from a block device onto an explicit target
//     directory (Linux-style: mount <dev> <dir>).
//
// Nothing is auto-mounted; the layout (which directories exist under "/") is
// registered by mntfs.

// Scan the blkdev table and expose every present device under /dev.
void vfsdev_init(void);

// Add/remove the /dev/<name> block node for a blkdev. Registration is
// idempotent and may happen before vfsdev_init() (devfs registration is
// order-independent). Unregister refuses while the device is mounted.
int vfsdev_register_block_device  (blkdev_t *bd);
int vfsdev_unregister_block_device(blkdev_t *bd);

// 1 if the device currently has a mounted filesystem.
int vfsdev_device_mounted(const char *devname);

// Mount the filesystem on `devarg` (accepts "nvme0", "/nvme0" or
// "/dev/nvme0") at the existing directory `target` using the loaded
// filesystem module named by `fstype` ("auto"/"*" probes all modules).
// Returns 0 on success, negative errno otherwise.
int vfsdev_mount(const char *devarg, const char *target, const char *fstype);

// Unmount by mount target path or by device name/device path.
int vfsdev_umount(const char *arg);

// Print the current device mounts.
void vfsdev_list(void);

#endif
