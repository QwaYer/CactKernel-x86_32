#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

// Block device layer limits.
// Physical (whole-disk) devices come from storage kmods; every disk may carry
// up to BLKDEV_PARTS_PER_DISK partition devices discovered by the partition
// layer. All of them live in one statically allocated table (BLKDEV_SLOTS),
// so pointers into it stay valid as long as the slot is not reused.
#define BLKDEV_NAME_MAX       16
#define BLKDEV_MAX_DISKS       8
#define BLKDEV_PARTS_PER_DISK 16
#define BLKDEV_SLOTS          (BLKDEV_MAX_DISKS * (1 + BLKDEV_PARTS_PER_DISK))

// Partition-table formats recognised by the partition layer.
#define PART_TABLE_NONE 0
#define PART_TABLE_MBR  1
#define PART_TABLE_GPT  2

// MBR partition types that describe containers rather than usable devices.
#define MBR_TYPE_EXTENDED 0x05
#define MBR_TYPE_EXTENDED_LBA 0x0F
#define MBR_TYPE_EXTENDED_WIN 0x85
#define MBR_TYPE_GPT_PROT 0xEE

// Generic block device descriptor. Whole-disk devices are registered by
// storage kmods (register_blkdev) and carry the controller callbacks;
// partition devices are created by the partition layer and reference their
// parent disk plus an LBA offset. Every device in the table has a stable
// numeric id == slot index (used by the page cache and fs modules).
typedef struct blkdev blkdev_t;
struct blkdev {
    char     name[BLKDEV_NAME_MAX];
    uint32_t id;               // stable slot index / device id
    uint32_t max_lba;          // capacity of THIS device in 512-byte sectors

    blkdev_t *parent;          // NULL for a physical disk
    uint32_t start_lba;        // partition start in parent sectors (0 = disk)
    uint32_t part_no;          // 1-based partition number (0 = whole disk)
    uint8_t  table;            // PART_TABLE_* for the source label (partitions)
    uint8_t  ptype;            // MBR partition type byte (0 otherwise)
    int      devfs_registered; // partition node present in devfs

    // Whole-disk I/O callbacks (set by register_blkdev only).
    void (*read_sector) (uint32_t lba, uint8_t *buf);
    void (*write_sector)(uint32_t lba, uint8_t *buf);
};

// Public API
void      blkdev_init        (void);
blkdev_t *blkdev_get_boot    (void);
blkdev_t *blkdev_find        (const char *name);
blkdev_t *blkdev_by_id       (uint32_t id);
int       blkdev_count       (void);       // total devices incl. partitions
int       blkdev_disk_count  (void);       // whole-disk devices only
void      blkdev_dump        (void);

// Per-device sector I/O. Partitions translate lba to (parent, start_lba+lba).
// Returns 0 on success, -1 on a missing device / out-of-range lba.
int blkdev_read (blkdev_t *dev, uint32_t lba, uint8_t *buf);
int blkdev_write(blkdev_t *dev, uint32_t lba, uint8_t *buf);

// Whole-disk convenience helpers used by swap / fallback drivers: read/write
// on the boot device (kept for compatibility with the pre-partition layer).
void blkdev_read_sector (uint32_t lba, uint8_t *buf);
void blkdev_write_sector(uint32_t lba, uint8_t *buf);

/* Register a whole-disk block device (typically from a storage kmod).
 * First successful registration becomes blkdev_get_boot(). */
int register_blkdev(const char *name, uint32_t max_lba,
                    void (*read_sector)(uint32_t lba, uint8_t *buf),
                    void (*write_sector)(uint32_t lba, uint8_t *buf));

void unregister_blkdev(const char *name);

// Hook invoked from register_blkdev() right after a whole-disk device appears
// (used by the partition layer to probe MBR/GPT labels on hotplug). Runs in
// the caller's context, so it must be a task context if it does I/O.
void blkdev_set_probe_hook(void (*fn)(blkdev_t *disk));

// Partition-layer API ------------------------------------------------------
// Add a partition sub-device of `disk`. Returns the new device or NULL.
blkdev_t *blkdev_add_partition(blkdev_t *disk, uint32_t part_no,
                               uint32_t start_lba, uint32_t len_lba,
                               uint8_t ptype, uint8_t table);

// Drop every partition of `disk` (blkdev entries only; devfs cleanup is the
// caller's responsibility via the partition layer). Returns count removed.
int blkdev_clear_partitions(blkdev_t *disk);

// Number of partitions currently attached to `disk` / fetch one by index.
int       blkdev_part_count(blkdev_t *disk);
blkdev_t *blkdev_part_at  (blkdev_t *disk, int idx);

// Compose the conventional partition device name: "<disk><n>" or, when the
// disk name already ends in a digit, "<disk>p<n>".
void blkdev_partition_name(const blkdev_t *disk, uint32_t part_no,
                           char *out, int cap);

#endif
