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

int blkdev_register(const char *name, uint32_t max_lba,
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

    kprint("[BLKDEV] registered ");
    kprint(d->name);
    kprint(" max_lba=");
    char buf[16];
    hex_to_ascii(max_lba, buf);
    kprint(buf);
    if (boot_dev == d)
        kprint(" *boot*\n");
    else
        kprint("\n");
    return 0;
}

void blkdev_unregister(const char *name) {
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

    kprint("[BLKDEV] unregistered ");
    kprint((char *)name);
    kprint("\n");
}

// Storage drivers call blkdev_register() during PCI probe (NVMe/AHCI kmods).
void blkdev_init(void) {
    dev_count = 0;
    boot_dev  = 0;
    memset(devices, 0, sizeof(devices));

    klog(LOG_OK, "Block device layer ready (drivers register at PCI probe)");
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
    kprint("[blkdev] Devices:\n");
    char b[16];
    for (int i = 0; i < dev_count; i++) {
        kprint("  "); kprint(devices[i].name);
        kprint(" lba="); hex_to_ascii(devices[i].max_lba, b); kprint(b);
        if (&devices[i] == boot_dev) kprint(" *boot*");
        kprint("\n");
    }
}

// Read a sector from the boot device (zeroes buffer if no device)
void blkdev_read_sector(uint32_t lba, uint8_t *buf) {
    if (!boot_dev) {
        kprint("[blkdev] no boot device for read\n");
        memset(buf, 0, 512);
        return;
    }
    boot_dev->read_sector(lba, buf);
}

// Write a sector to the boot device (no-op if no device)
void blkdev_write_sector(uint32_t lba, uint8_t *buf) {
    if (!boot_dev) {
        kprint("[blkdev] no boot device for write\n");
        return;
    }
    boot_dev->write_sector(lba, buf);
}