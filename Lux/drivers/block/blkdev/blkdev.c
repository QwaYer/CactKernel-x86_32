#include "blkdev.h"
#include "kernel.h"
#include "libc.h"
#include "memory.h"
#include "nvme.h"
#include "ahci.h"

static blkdev_t devices[BLKDEV_MAX];
static int      dev_count = 0;
static blkdev_t *boot_dev = 0;

static blkdev_t *_add(const char *name,
                      void (*rd)(uint32_t, uint8_t*),
                      void (*wr)(uint32_t, uint8_t*),
                      uint32_t max_lba) {
    if (dev_count >= BLKDEV_MAX) {
        kprint("[blkdev] table full\n");
        return 0;
    }
    blkdev_t *d = &devices[dev_count++];
    memset(d, 0, sizeof(blkdev_t));
    strlcpy(d->name, name, BLKDEV_NAME_MAX);
    d->read_sector  = rd;
    d->write_sector = wr;
    d->max_lba      = max_lba;
    return d;
}

static void _probe_nvme(void) {
    extern int nvme_is_ready(void);
    extern uint32_t nvme_get_max_lba(void);

    nvme_init();
    if (!nvme_is_ready()) {
        kprint("[BLKDEV] NVMe controller not ready\n");
        return;
    }

    blkdev_t *d = _add("nvme0", nvme_read_sector, nvme_write_sector,
                        nvme_get_max_lba());
    if (d) boot_dev = d;
}

static void _ahci_rd(uint32_t lba, uint8_t *buf) {
    ahci_read_sector((uint32_t)ahci_first_port(), lba, buf);
}

static void _ahci_wr(uint32_t lba, uint8_t *buf) {
    ahci_write_sector((uint32_t)ahci_first_port(), lba, buf);
}

static void _probe_ahci(void) {
    ahci_init();
    if (ahci_first_port() < 0) {
        kprint("[BLKDEV] AHCI: no active ports\n");
        return;
    }

    extern uint32_t ahci_get_max_lba(int port);
    int port = ahci_first_port();

    blkdev_t *d = _add("sda", _ahci_rd, _ahci_wr,
                        ahci_get_max_lba(port));
    if (d && !boot_dev) boot_dev = d;
}

//public api
void blkdev_init(void) {
    kprint("[BLKDEV] probing AHCI (SATA)\n");
    dev_count = 0;
    boot_dev  = 0;
    memset(devices, 0, sizeof(devices));

    _probe_ahci();
    if (dev_count > 0)
        kprint("[BLKDEV] AHCI: sda registered\n");
    else
        kprint("[BLKDEV] AHCI: no drives found\n");

    kprint("[BLKDEV] probing NVMe\n");
    _probe_nvme();
    if (boot_dev && boot_dev->read_sector == nvme_read_sector)
        kprint("[BLKDEV] NVMe: nvme0 registered\n");

    if (boot_dev) {
        kprint("[BLKDEV] boot device: "); kprint(boot_dev->name);
        kprint("  max_lba="); char buf[16]; hex_to_ascii(boot_dev->max_lba, buf); kprint(buf);
        kprint("\n");
    } else {
        kprint_color("[BLKDEV] no boot device found — filesystem mounts will fail\n",
                     COLOR_LIGHT_RED);
    }
}

blkdev_t *blkdev_get_boot(void) {
    return boot_dev;
}

blkdev_t *blkdev_find(const char *name) {
    for (int i = 0; i < dev_count; i++)
        if (strcmp(devices[i].name, (char*)name) == 0)
            return &devices[i];
    return 0;
}

int blkdev_count(void) {
    return dev_count;
}

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

void blkdev_read_sector(uint32_t lba, uint8_t *buf) {
    if (!boot_dev) {
        kprint("[blkdev] no boot device for read\n");
        memset(buf, 0, 512);
        return;
    }
    boot_dev->read_sector(lba, buf);
}

void blkdev_write_sector(uint32_t lba, uint8_t *buf) {
    if (!boot_dev) {
        kprint("[blkdev] no boot device for write\n");
        return;
    }
    boot_dev->write_sector(lba, buf);
}