#include "vfsdev.h"
#include "devfs.h"
#include "vfs.h"
#include "blkdev.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"

// vfsdev.c — block device nodes under /dev.
//
// Every blkdev (whole disk and partition) is exposed as a single
// VFS_BLOCKDEVICE node (/dev/<name>) whose reads/writes map byte offsets
// onto the device through blkdev_read/blkdev_write.  Registration happens
// from the partition probe hook (whole disks on register_blkdev, partitions
// as they are created) and is order-independent with respect to devfs init.

static int blk_node_count = 0;   // block nodes currently exposed in /dev

// Byte-range rw on any block device (whole disk or partition). Works sector
// by sector so unaligned offsets behave like the storage drivers do.
static int blk_byte_rw(blkdev_t *bd, uint32_t off, uint32_t size,
                       char *buf, int wr) {
    if (!bd || !buf) return -1;
    if (size == 0) return 0;

    uint64_t total = (uint64_t)bd->max_lba * 512ull;
    if ((uint64_t)off >= total) return 0;
    if ((uint64_t)off + size > total) size = (uint32_t)(total - off);

    uint8_t sec[512];
    uint32_t done = 0;
    uint32_t first = off / 512;
    uint32_t soff  = off % 512;

    for (uint32_t i = 0; done < size; i++) {
        uint32_t lba = first + i;
        uint32_t want  = size - done;
        uint32_t avail = 512 - soff;
        uint32_t chunk = (want < avail) ? want : avail;
        int full = (soff == 0 && chunk == 512);

        if (wr) {
            if (full) {
                memcpy(sec, buf + done, 512);
                if (blkdev_write(bd, lba, sec) != 0) break;
            } else {
                memset(sec, 0, 512);
                if (blkdev_read(bd, lba, sec) != 0) memset(sec, 0, 512);
                memcpy(sec + soff, buf + done, chunk);
                if (blkdev_write(bd, lba, sec) != 0) break;
            }
        } else {
            memset(sec, 0, 512);
            if (blkdev_read(bd, lba, sec) != 0) break;
            memcpy(buf + done, sec + soff, chunk);
        }

        done += chunk;
        soff  = 0;
    }
    return (int)done;
}

static int _blk_read(void *priv, uint32_t off, uint32_t size, char *buf) {
    return blk_byte_rw((blkdev_t *)priv, off, size, buf, 0);
}

static int _blk_write(void *priv, uint32_t off, uint32_t size, char *buf) {
    return blk_byte_rw((blkdev_t *)priv, off, size, buf, 1);
}

static int _blk_status(void *priv, char *buf, uint32_t size) {
    blkdev_t *bd = (blkdev_t *)priv;
    if (!bd || !buf || size == 0) return 0;
    char tmp[192];
    int n = 0;
    const char *s;
    s = "device: "; for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    for (int i = 0; bd->name[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = bd->name[i];
    s = bd->parent ? "\ntype: partition\n" : "\ntype: block device\n";
    for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    if (bd->parent) {
        s = "disk: "; for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
        for (int i = 0; bd->parent->name[i] && n < (int)sizeof(tmp) - 1; i++)
            tmp[n++] = bd->parent->name[i];
    }
    {
        char nb[16];
        s = "\nsize_lba: "; for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
        snprintf(nb, sizeof(nb), "%u", (unsigned)(bd->max_lba));
        for (int i = 0; nb[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = nb[i];
    }
    if (n < (int)sizeof(tmp) - 1) tmp[n++] = '\n';
    tmp[n] = '\0';
    if ((uint32_t)n > size) n = (int)size;
    memcpy(buf, tmp, (uint32_t)n);
    return n;
}

static devfs_driver_t drv_blk = {
    .read   = _blk_read,
    .write  = _blk_write,
    .status = _blk_status,
};

// Return the device node size in bytes (saturating to 32-bit range).
static uint32_t blk_node_size(blkdev_t *bd) {
    uint64_t bytes = (uint64_t)bd->max_lba * 512ull;
    return bytes < 0x100000000ull ? (uint32_t)bytes : 0xFFFFFFFFu;
}

int vfsdev_register_block_device(blkdev_t *bd) {
    if (!bd || !bd->name[0]) return -1;
    if (devfs_find(bd->name)) return 0;   // already exposed

    devfs_entry_t *e = register_chrdev(bd->name,
                                       DEVFS_F_SIMPLE | DEVFS_F_BLOCK,
                                       &drv_blk, bd);
    if (!e) return -1;
    e->dir_node.size = blk_node_size(bd);
    blk_node_count++;
    return 0;
}

int vfsdev_unregister_block_device(blkdev_t *bd) {
    if (!bd) return -1;
    if (vfsdev_device_mounted(bd->name)) return -2;  // busy
    int r = unregister_chrdev(bd->name);
    if (r == 0 && blk_node_count > 0) blk_node_count--;
    return r;
}

// Register every device currently present in the blkdev table. Devices added
// later (hotplug) are handled by the partition probe hook.
void vfsdev_init(void) {
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        blkdev_t *bd = blkdev_by_id((uint32_t)i);
        if (bd) vfsdev_register_block_device(bd);
    }
    pr_info("  %-11s : %d block device node(s) exposed in /dev\n",
            "vfsdev", blk_node_count);
}
