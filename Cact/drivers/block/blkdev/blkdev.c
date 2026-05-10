#include "blkdev.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"

// Static device table and boot device pointer
static blkdev_t devices[BLKDEV_MAX];
static int      dev_count = 0;
static blkdev_t *boot_dev = 0;

// Storage HBA drivers are loaded as kmod modules post-boot.
void blkdev_init(void) {
    dev_count = 0;
    boot_dev  = 0;
    memset(devices, 0, sizeof(devices));

    kprint("[BLKDEV] no built-in boot storage drivers\n");

    if (boot_dev) {
        kprint("[BLKDEV] boot device: "); kprint(boot_dev->name);
        kprint("  max_lba="); char buf[16]; hex_to_ascii(boot_dev->max_lba, buf); kprint(buf);
        kprint("  total="); itoa((int)((uint64_t)boot_dev->max_lba * 512 / 1024 / 1024), buf);
        kprint(buf); kprint(" MB\n");
        klog(LOG_OK,  "block device layer ready");
    } else {
        kprint_color("[BLKDEV] no boot device found — filesystem mounts will fail\n",
                     COLOR_LIGHT_RED);
        klog(LOG_WARN, "no boot disk — storage unavailable");
    }
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