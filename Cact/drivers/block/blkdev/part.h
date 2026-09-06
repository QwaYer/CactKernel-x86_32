#ifndef PART_H
#define PART_H

#include "blkdev.h"

// Partition layer: MBR/GPT parsing on top of the blkdev table.
//
// Scan produces one blkdev_t sub-device per partition ("sda1", "nvme0p1" ...)
// plus a matching devfs node (complex block device with data/status) so that
// userland can read/write raw partition sectors. Rescan of a disk drops the
// old partition devices first, so an installer can rewrite a partition table
// and ask the kernel to re-expose it without a reboot.

// Scan one whole-disk device. Creates partition blkdev entries and their
// devfs nodes. Returns the number of partitions found (0 if none / no label),
// or a negative error code.
int part_scan_disk(blkdev_t *disk);

// Arm automatic scanning: every register_blkdev() of a whole disk triggers
// part_scan_disk() on it (hotplug + late kmod load).
void part_probe_init(void);

// Drop partitions of `disk` (devfs nodes + blkdev entries).
int part_drop_disk(blkdev_t *disk);

// Scan all currently registered whole-disk devices. Called at boot after the
// storage kmods have been probed. Returns total partitions created.
int part_scan_all(void);

// Scan a single disk by name (used by the /dev/sys rescan ioctl).
int part_rescan(const char *name);

#endif
