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

// ── /dev/disk/by-id links ────────────────────────────────────────────────
// A partition created before its disk's identity was known derives one from
// the parent, so both the publish and the remove path agree on the name.
static int _by_id_path(blkdev_t *bd, char *out, int cap) {
    if (!bd) return 0;
    if (bd->ident[0]) {
        snprintf(out, cap, "disk/by-id/%s", bd->ident);
        return 1;
    }
    if (bd->parent && bd->parent->ident[0]) {
        snprintf(out, cap, "disk/by-id/%s-part%u",
                 bd->parent->ident, (unsigned)bd->part_no);
        return 1;
    }
    return 0;
}

static void _publish_by_id(blkdev_t *bd) {
    char link[96];
    if (!_by_id_path(bd, link, sizeof(link))) return;
    char target[BLKDEV_NAME_MAX + 8];
    // Relative to the link's directory (/dev/disk/by-id) up to /dev/<name>,
    // exactly like Linux's ../../sda targets.
    snprintf(target, sizeof(target), "../../%s", bd->name);
    devfs_symlink(link, target);
}

void vfsdev_set_block_ident(blkdev_t *bd) {
    if (!bd) return;
    _publish_by_id(bd);
    for (int i = 0; i < blkdev_part_count(bd); i++) {
        blkdev_t *p = blkdev_part_at(bd, i);
        if (p) _publish_by_id(p);
    }
}

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

static devfs_driver_t drv_blk = {
    .read   = _blk_read,
    .write  = _blk_write,
};

// Return the device node size in bytes (saturating to 32-bit range).
static uint32_t blk_node_size(blkdev_t *bd) {
    uint64_t bytes = (uint64_t)bd->max_lba * 512ull;
    return bytes < 0x100000000ull ? (uint32_t)bytes : 0xFFFFFFFFu;
}

int vfsdev_register_block_device(blkdev_t *bd) {
    if (!bd || !bd->name[0]) return -1;

    devfs_entry_t *exist = devfs_find(bd->name);
    if (exist) {
        /* A storage kmod may have registered the node itself (to keep its own
         * status op); the block layer still owns the geometry, so stamp the
         * size the driver cannot know and refresh the by-id link.  Without
         * this /dev/sda reports size 0 while /dev/sda1 is correct. */
        if (!(exist->flags & DEVFS_F_LINK))
            exist->node.size = blk_node_size(bd);
        _publish_by_id(bd);
        return 0;
    }

    // Block devices are flat nodes in /dev (/dev/sda) — the Linux layout.
    // /dev/disk holds only the by-id/by-path symlinks pointing back here; it
    // never contains a real device node.
    devfs_entry_t *e = register_chrdev(bd->name,
                                       DEVFS_F_BLOCK,
                                       &drv_blk, bd);
    if (!e) return -1;
    e->node.size = blk_node_size(bd);
    blk_node_count++;

    _publish_by_id(bd);           // /dev/disk/by-id/<ident> -> ../../<name>
    return 0;
}

int vfsdev_unregister_block_device(blkdev_t *bd) {
    if (!bd) return -1;
    if (vfsdev_device_mounted(bd->name)) return -2;  // busy

    char link[96];
    if (_by_id_path(bd, link, sizeof(link)))
        devfs_remove(link);

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
    pr_info("%s: %d block device node(s) exposed in /dev\n",
            "vfsdev", blk_node_count);
}
