#include "blkdev.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"

// Static device table and boot device pointer. Slots are never compacted:
// blkdev_t* handed to fs modules / page cache must stay valid for the life
// of the mount, and device ids == slot indexes. Freed slots are reused.
static blkdev_t devices[BLKDEV_SLOTS];
static int      dev_used[BLKDEV_SLOTS];
static int      dev_count   = 0;   // live devices
static blkdev_t *boot_dev   = 0;

// Partition-layer probe (set by part_probe_init); called when a disk appears.
static void (*probe_hook)(blkdev_t *disk) = 0;

void blkdev_set_probe_hook(void (*fn)(blkdev_t *disk)) {
    probe_hook = fn;
}

static int alloc_slot(void) {
    for (int i = 0; i < BLKDEV_SLOTS; i++)
        if (!dev_used[i])
            return i;
    return -1;
}

static void blkdev_pick_boot(void) {
    boot_dev = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        if (dev_used[i] && !devices[i].parent) {
            boot_dev = &devices[i];
            return;
        }
    }
}

// Is `dev` a whole-disk device (owns controller callbacks)?
static int blkdev_is_disk(const blkdev_t *d) {
    return d && d->parent == 0;
}

int register_blkdev(const char *name, uint32_t max_lba,
                    void (*read_sector)(uint32_t lba, uint8_t *buf),
                    void (*write_sector)(uint32_t lba, uint8_t *buf)) {
    if (!name || !name[0] || !read_sector || !write_sector)
        return -1;
    if (blkdev_find(name))
        return -3;

    int idx = alloc_slot();
    if (idx < 0)
        return -2;

    blkdev_t *d = &devices[idx];
    memset(d, 0, sizeof *d);
    strncpy(d->name, (char *)name, BLKDEV_NAME_MAX - 1);
    d->name[BLKDEV_NAME_MAX - 1] = '\0';
    d->id          = (uint32_t)idx;
    d->max_lba     = max_lba;
    d->parent      = 0;
    d->start_lba   = 0;
    d->part_no     = 0;
    d->read_sector   = read_sector;
    d->write_sector  = write_sector;
    dev_used[idx] = 1;
    dev_count++;

    if (!boot_dev)
        boot_dev = d;

    printk("[BLKDEV] registered ");
    printk(d->name);
    printk(" max_lba=");
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(max_lba));
    printk(buf);
    if (boot_dev == d)
        printk(" *boot*\n");
    else
        printk("\n");

    // Let the partition layer look at the new disk immediately (hotplug and
    // late-loaded storage kmods both end up here).
    if (probe_hook)
        probe_hook(d);
    return 0;
}

void unregister_blkdev(const char *name) {
    if (!name)
        return;
    blkdev_t *d = blkdev_find(name);
    if (!d || !blkdev_is_disk(d))
        return;

    // Drop this disk's partitions first (devfs entries are cleaned by the
    // partition layer; here we only release the blkdev slots).
    blkdev_clear_partitions(d);

    int idx = (int)d->id;
    dev_used[idx] = 0;
    memset(&devices[idx], 0, sizeof(blkdev_t));
    dev_count--;

    blkdev_pick_boot();

    printk("[BLKDEV] unregistered ");
    printk((char *)name);
    printk("\n");
}

// Storage drivers call register_blkdev() during PCI probe (NVMe/AHCI kmods).
void blkdev_init(void) {
    memset(devices,   0, sizeof(devices));
    memset(dev_used,  0, sizeof(dev_used));
    dev_count = 0;
    boot_dev  = 0;

    pr_info("  %-11s : layer ready (kmods attach during PCI probe)\n", "block");
}

// Return the boot device (first successfully probed whole disk)
blkdev_t *blkdev_get_boot(void) {
    return boot_dev;
}

// Find a device by name (linear scan), returns NULL if not found
blkdev_t *blkdev_find(const char *name) {
    if (!name)
        return 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++)
        if (dev_used[i] && strcmp(devices[i].name, (char *)name) == 0)
            return &devices[i];
    return 0;
}

blkdev_t *blkdev_by_id(uint32_t id) {
    if (id >= (uint32_t)BLKDEV_SLOTS)
        return 0;
    return dev_used[(int)id] ? &devices[(int)id] : 0;
}

// Return number of registered block devices
int blkdev_count(void) {
    return dev_count;
}

int blkdev_disk_count(void) {
    int n = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++)
        if (dev_used[i] && blkdev_is_disk(&devices[i]))
            n++;
    return n;
}

// Print all registered devices with LBA and boot flag
void blkdev_dump(void) {
    printk("[blkdev] Devices:\n");
    char b[16];
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        if (!dev_used[i])
            continue;
        blkdev_t *d = &devices[i];
        printk("  "); printk(d->name);
        if (!blkdev_is_disk(d)) {
            printk(" [part of ");
            printk(d->parent ? d->parent->name : "?");
            printk(" @ ");
            snprintf(b, sizeof(b), "0x%x", (unsigned)(d->start_lba));
            printk(b);
            printk("]");
        }
        printk(" lba=");
        snprintf(b, sizeof(b), "0x%x", (unsigned)(d->max_lba));
        printk(b);
        if (d == boot_dev) printk(" *boot*");
        printk("\n");
    }
}

// Returns true if lba is out of range; prints diagnostic
static int blkdev_lba_oob(const char *op, uint32_t lba, uint32_t max_lba) {
    if (lba < max_lba) return 0;
    printk("[blkdev] "); printk(op); printk(" out of range: lba=");
    char _b[16]; snprintf(_b, sizeof(_b), "0x%x", (unsigned)(lba)); printk(_b);
    printk(" >= max_lba=");
    snprintf(_b, sizeof(_b), "0x%x", (unsigned)(max_lba)); printk(_b);
    printk("\n");
    return 1;
}

// Per-device sector I/O. Partition devices map their own LBA space into the
// parent disk at start_lba; the controller callbacks live on the disk.
int blkdev_read(blkdev_t *dev, uint32_t lba, uint8_t *buf) {
    if (!dev || !buf)
        return -1;
    if (blkdev_lba_oob("read", lba, dev->max_lba))
        return -1;
    if (blkdev_is_disk(dev)) {
        if (!dev->read_sector)
            return -1;
        dev->read_sector(lba, buf);
        return 0;
    }
    if (!dev->parent)
        return -1;
    uint64_t plba = (uint64_t)dev->start_lba + lba;
    if (plba > 0xFFFFFFFFull)
        return -1;
    return blkdev_read(dev->parent, (uint32_t)plba, buf);
}

int blkdev_write(blkdev_t *dev, uint32_t lba, uint8_t *buf) {
    if (!dev || !buf)
        return -1;
    if (blkdev_lba_oob("write", lba, dev->max_lba))
        return -1;
    if (blkdev_is_disk(dev)) {
        if (!dev->write_sector)
            return -1;
        dev->write_sector(lba, buf);
        return 0;
    }
    if (!dev->parent)
        return -1;
    uint64_t plba = (uint64_t)dev->start_lba + lba;
    if (plba > 0xFFFFFFFFull)
        return -1;
    return blkdev_write(dev->parent, (uint32_t)plba, buf);
}

// Compatibility helpers: read/write a sector on the boot device
// (zeroes buffer / no-op if no device).
void blkdev_read_sector(uint32_t lba, uint8_t *buf) {
    blkdev_t *dev = boot_dev;
    if (!dev) {
        printk("[blkdev] no boot device for read\n");
        memset(buf, 0, 512);
        return;
    }
    if (blkdev_read(dev, lba, buf) != 0) {
        memset(buf, 0, 512);
        return;
    }
}

void blkdev_write_sector(uint32_t lba, uint8_t *buf) {
    blkdev_t *dev = boot_dev;
    if (!dev) {
        printk("[blkdev] no boot device for write\n");
        return;
    }
    (void)blkdev_write(dev, lba, buf);
}

// ── Partition sub-devices ─────────────────────────────────────────────────

// Compose the conventional partition device name.
void blkdev_partition_name(const blkdev_t *disk, uint32_t part_no,
                           char *out, int cap) {
    if (!disk || !out || cap <= 0) {
        if (out && cap > 0) out[0] = '\0';
        return;
    }
    int l = 0;
    while (disk->name[l] && l < BLKDEV_NAME_MAX) l++;
    int digit = (l > 0 && disk->name[l-1] >= '0' && disk->name[l-1] <= '9');

    char num[12];
    snprintf(num, sizeof(num), "%u", (unsigned)part_no);

    int o = 0;
    for (int i = 0; i < l && o < cap - 1; i++)
        out[o++] = disk->name[i];
    if (digit)
        out[o++] = 'p';
    for (int i = 0; num[i] && o < cap - 1; i++)
        out[o++] = num[i];
    out[o] = '\0';
}

blkdev_t *blkdev_add_partition(blkdev_t *disk, uint32_t part_no,
                               uint32_t start_lba, uint32_t len_lba,
                               uint8_t ptype, uint8_t table) {
    if (!disk || !blkdev_is_disk(disk) || part_no == 0 || len_lba == 0)
        return 0;
    if (blkdev_part_count(disk) >= BLKDEV_PARTS_PER_DISK)
        return 0;

    // Ensure the partition actually lies inside the disk.
    if (start_lba >= disk->max_lba || len_lba > disk->max_lba - start_lba) {
        printk("[BLKDEV] partition out of disk range, skipped\n");
        return 0;
    }

    char name[BLKDEV_NAME_MAX];
    blkdev_partition_name(disk, part_no, name, sizeof(name));
    if (blkdev_find(name)) {
        printk("[BLKDEV] duplicate partition device: "); printk(name); printk("\n");
        return 0;
    }

    int idx = alloc_slot();
    if (idx < 0) {
        printk("[BLKDEV] no free slot for partition\n");
        return 0;
    }

    blkdev_t *d = &devices[idx];
    memset(d, 0, sizeof *d);
    strncpy(d->name, name, BLKDEV_NAME_MAX - 1);
    d->name[BLKDEV_NAME_MAX - 1] = '\0';
    d->id          = (uint32_t)idx;
    d->max_lba     = len_lba;
    d->parent      = disk;
    d->start_lba   = start_lba;
    d->part_no     = part_no;
    d->ptype       = ptype;
    d->table       = table;
    dev_used[idx] = 1;
    dev_count++;

    printk("[BLKDEV] partition ");
    printk(name);
    printk(": start=");
    char b[16];
    snprintf(b, sizeof(b), "0x%x", (unsigned)(start_lba));
    printk(b);
    printk(" len=");
    snprintf(b, sizeof(b), "0x%x", (unsigned)(len_lba));
    printk(b);
    printk(" on ");
    printk(disk->name);
    printk("\n");
    return d;
}

int blkdev_clear_partitions(blkdev_t *disk) {
    if (!disk)
        return 0;
    int n = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        if (!dev_used[i])
            continue;
        if (devices[i].parent != disk)
            continue;
        dev_used[i] = 0;
        memset(&devices[i], 0, sizeof(blkdev_t));
        dev_count--;
        n++;
    }
    return n;
}

int blkdev_part_count(blkdev_t *disk) {
    if (!disk)
        return 0;
    int n = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++)
        if (dev_used[i] && devices[i].parent == disk)
            n++;
    return n;
}

blkdev_t *blkdev_part_at(blkdev_t *disk, int idx) {
    if (!disk || idx < 0)
        return 0;
    int n = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        if (!dev_used[i] || devices[i].parent != disk)
            continue;
        if (n++ == idx)
            return &devices[i];
    }
    return 0;
}
