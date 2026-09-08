#ifndef MNTFS_H
#define MNTFS_H

#include <stdint.h>
#include "vfs.h"

// mntfs — Linux-style root layout registrar.
//
// mntfs does NOT own devices or the mount manager (that is vfsdev).  It only
// decides what "/" is and registers which directories exist under it:
//
//   * with a boot block device whose filesystem mounts: "/" is that
//     filesystem root (ext4 of the boot disk);
//   * otherwise "/" is a RAM rootfs (tmpfs instance);
//
// then the standard directories are created on the root and the subsystem
// filesystems are mounted on top:
//
//   /bin /sbin /lib /usr /etc /var   overlay cctkfs-on-ext4 (or RAM userland)
//   /tmp  tmpfs
//   /dev  devfs (char/service nodes) + vfsdev block nodes
//   /proc procfs
//   /home /mnt   plain directories on the root filesystem

// Initialise the VFS root and register the standard directory layout.
void mntfs_init(void);

#endif
