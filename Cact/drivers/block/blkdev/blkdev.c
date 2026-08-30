#include "blkdev.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"

// Static device table and boot device pointer
static blkdev_t devices[BLKDEV_MAX];
static int      dev_count = 0;
static blkdev_t *boot_dev = 0;

static void blkdev_pick_boot(void) {
    boot_dev = dev_count > 0 ? &devices[0] : 0;
}

int register_blkdev(const char *name, uint32_t max_lba,
                    void (*read_sector)(uint32_t lba, uint8_t *buf),
                    void (*write_sector)(uint32_t lba, uint8_t *buf)) {
    if (!name || !name[0] || !read_sector || !write_sector)
        return -1;
    if (dev_count >= BLKDEV_MAX)
        return -2;
    if (blkdev_find(name))
        return -3;

    blkdev_t *d = &devices[dev_count];
    memset(d, 0, sizeof *d);
    strncpy(d->name, (char *)name, BLKDEV_NAME_MAX - 1);
    d->name[BLKDEV_NAME_MAX - 1] = '\0';
    d->max_lba       = max_lba;
    d->read_sector   = read_sector;
    d->write_sector  = write_sector;
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
    return 0;
}

void unregister_blkdev(const char *name) {
    if (!name)
        return;
    int idx = -1;
    for (int i = 0; i < dev_count; i++) {
        if (strcmp(devices[i].name, (char *)name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;

    for (int i = idx; i < dev_count - 1; i++)
        devices[i] = devices[i + 1];
    dev_count--;
    memset(&devices[dev_count], 0, sizeof(blkdev_t));

    blkdev_pick_boot();

    printk("[BLKDEV] unregistered ");
    printk((char *)name);
    printk("\n");
}

// Storage drivers call register_blkdev() during PCI probe (NVMe/AHCI kmods).
void blkdev_init(void) {
    dev_count = 0;
    boot_dev  = 0;
    memset(devices, 0, sizeof(devices));

    pr_info("Block device layer ready (drivers register at PCI probe)");
}

// Return the boot device (first successfully probed drive)
blkdev_t *blkdev_get_boot(void) {
    return boot_dev;
}

// Find a device by name (linear scan), returns NULL if not found
blkdev_t *blkdev_find(const char *name) {
    for (int i = 0; i < dev_count; i++)
        if (strcmp(devices[i].name, (char*)name) == 0)
            return &devices[i];
    return 0;
}

// Return number of registered block devices
int blkdev_count(void) {
    return dev_count;
}

// Print all registered devices with LBA and boot flag
void blkdev_dump(void) {
    printk("[blkdev] Devices:\n");
    char b[16];
    for (int i = 0; i < dev_count; i++) {
        printk("  "); printk(devices[i].name);
        printk(" lba="); snprintf(b, sizeof(b), "0x%x", (unsigned)(devices[i].max_lba)); printk(b);
        if (&devices[i] == boot_dev) printk(" *boot*");
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

// Read a sector from the boot device (zeroes buffer if no device)
void blkdev_read_sector(uint32_t lba, uint8_t *buf) {
    struct blkdev *dev = boot_dev;
    if (!dev) {
        printk("[blkdev] no boot device for read\n");
        memset(buf, 0, 512);
        return;
    }
    if (blkdev_lba_oob("read", lba, dev->max_lba)) {
        memset(buf, 0, 512);
        return;
    }
    dev->read_sector(lba, buf);
}

// Write a sector to the boot device (no-op if no device)
void blkdev_write_sector(uint32_t lba, uint8_t *buf) {
    struct blkdev *dev = boot_dev;
    if (!dev) {
        printk("[blkdev] no boot device for write\n");
        return;
    }
    if (blkdev_lba_oob("write", lba, dev->max_lba))
        return;
    dev->write_sector(lba, buf);
}