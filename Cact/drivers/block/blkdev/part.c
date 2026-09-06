/*
 * Partition layer (MBR + GPT) for the Cact blkdev layer.
 *
 * Every whole-disk blkdev_t may carry up to BLKDEV_PARTS_PER_DISK partition
 * sub-devices.  Scanning reads the disk label through the parent device's own
 * I/O callbacks (blkdev_read), so it works for any storage driver and must
 * run in a task context (reads may sleep on controller IRQs).
 *
 * Each created partition gets:
 *   - a blkdev_t sub-device  (blkdev_add_partition) with start_lba offset,
 *   - a devfs block node     ("/dev/sda1" style) whose data node translates
 *     byte offsets back to partition-relative sectors.
 */

#include "blkdev.h"
#include "part.h"
#include "devfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"

// GPT on-disk signature.
#define GPT_SIGNATURE "EFI PART"

#define MBR_PTABLE_OFF 446
#define MBR_PART_SIZE  16
#define MBR_NPARTS     4

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_len;
} __attribute__((packed)) mbr_part_t;

typedef struct {
    uint64_t signature;        // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc;
} __attribute__((packed)) gpt_hdr_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];         // UTF-16LE, 72 bytes
} __attribute__((packed)) gpt_entry_t;

// ── helpers ───────────────────────────────────────────────────────────────

static int all_zero16(const uint8_t *p) {
    for (int i = 0; i < 16; i++)
        if (p[i]) return 0;
    return 1;
}

static int mem_eq(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

static int mbr_type_extended(uint8_t t) {
    return t == MBR_TYPE_EXTENDED || t == MBR_TYPE_EXTENDED_LBA ||
           t == MBR_TYPE_EXTENDED_WIN;
}

// ── devfs driver for partition block devices ──────────────────────────────

// Byte-range rw on a partition device. Works sector by sector so unaligned
// read/write offsets behave like the whole-disk drivers do.
static int part_rw(blkdev_t *bd, uint32_t off, uint32_t size,
                   char *buf, int wr) {
    if (!bd || !buf) return -1;
    if (size == 0) return 0;

    uint8_t sec[512];
    uint32_t done = 0;
    uint32_t first = off / 512;
    uint32_t soff  = off % 512;

    for (uint32_t i = 0; done < size; i++) {
        uint32_t lba = first + i;
        uint32_t want = size - done;
        uint32_t avail = 512 - soff;
        uint32_t chunk = (want < avail) ? want : avail;
        int full = (soff == 0 && chunk == 512);

        if (wr) {
            if (full) {
                memcpy(sec, buf + done, 512);
                if (blkdev_write(bd, lba, sec) != 0)
                    break;
            } else {
                memset(sec, 0, 512);
                if (blkdev_read(bd, lba, sec) != 0) {
                    memset(sec, 0, 512);
                }
                memcpy(sec + soff, buf + done, chunk);
                if (blkdev_write(bd, lba, sec) != 0)
                    break;
            }
        } else {
            memset(sec, 0, 512);
            if (blkdev_read(bd, lba, sec) != 0)
                break;
            memcpy(buf + done, sec + soff, chunk);
        }

        done += chunk;
        soff  = 0;
    }
    return (int)done;
}

static int part_node_read(void *priv, uint32_t off, uint32_t size, char *buf) {
    return part_rw((blkdev_t *)priv, off, size, buf, 0);
}

static int part_node_write(void *priv, uint32_t off, uint32_t size, char *buf) {
    return part_rw((blkdev_t *)priv, off, size, buf, 1);
}

static int part_node_status(void *priv, char *buf, uint32_t size) {
    blkdev_t *bd = (blkdev_t *)priv;
    if (!bd) return 0;
    char tmp[256];
    int n = 0;
    const char *s;
    s = "device: ";  for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    for (int i = 0; bd->name[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = bd->name[i];
    s = "\ntype: partition\n";
    for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    s = "disk: ";  for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    if (bd->parent)
        for (int i = 0; bd->parent->name[i] && n < (int)sizeof(tmp) - 1; i++)
            tmp[n++] = bd->parent->name[i];
    s = "\nstart_lba: ";
    for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    {
        char nb[16];
        snprintf(nb, sizeof(nb), "%u", (unsigned)(bd->start_lba));
        for (int i = 0; nb[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = nb[i];
    }
    s = "\nsize_lba: ";
    for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    {
        char nb[16];
        snprintf(nb, sizeof(nb), "%u", (unsigned)(bd->max_lba));
        for (int i = 0; nb[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = nb[i];
    }
    s = "\ntable: ";
    for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    s = (bd->table == PART_TABLE_GPT) ? "gpt" :
        (bd->table == PART_TABLE_MBR) ? "mbr" : "none";
    for (; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    if (n < (int)sizeof(tmp) - 1) tmp[n++] = '\n';
    tmp[n] = '\0';

    if ((uint32_t)n > size) n = (int)size;
    memcpy(buf, tmp, (uint32_t)n);
    return n;
}

static devfs_driver_t drv_blkpart = {
    .read   = part_node_read,
    .write  = part_node_write,
    .status = part_node_status,
};

// ── partition registration / teardown ─────────────────────────────────────

static void part_devfs_add(blkdev_t *p) {
    if (p->devfs_registered)
        return;
    if (register_chrdev(p->name, DEVFS_F_BLOCK, &drv_blkpart, p))
        p->devfs_registered = 1;
}

static void part_devfs_remove(blkdev_t *p) {
    if (!p->devfs_registered)
        return;
    unregister_chrdev(p->name);
    p->devfs_registered = 0;
}

int part_drop_disk(blkdev_t *disk) {
    if (!disk)
        return -1;
    // Unregister devfs nodes first, then release the blkdev slots.
    for (int i = 0; i < blkdev_part_count(disk); i++) {
        blkdev_t *p = blkdev_part_at(disk, i);
        if (p)
            part_devfs_remove(p);
    }
    blkdev_clear_partitions(disk);
    return 0;
}

static blkdev_t *part_create(blkdev_t *disk, uint32_t part_no,
                             uint64_t start_lba, uint64_t len_lba,
                             uint8_t ptype, uint8_t table) {
    if (start_lba > 0xFFFFFFFFull || len_lba > 0xFFFFFFFFull)
        return 0;
    blkdev_t *p = blkdev_add_partition(disk, part_no,
                                       (uint32_t)start_lba, (uint32_t)len_lba,
                                       ptype, table);
    if (!p)
        return 0;
    part_devfs_add(p);
    return p;
}

// ── MBR parsing ───────────────────────────────────────────────────────────

static int part_parse_mbr(blkdev_t *disk, const uint8_t *lba0, int *found) {
    int n = 0;
    for (int i = 0; i < MBR_NPARTS; i++) {
        const uint8_t *e = lba0 + MBR_PTABLE_OFF + i * MBR_PART_SIZE;
        mbr_part_t pe;
        memcpy(&pe, e, sizeof(pe));

        if (pe.type == 0 || pe.lba_start == 0 || pe.lba_len == 0)
            continue;
        if (mbr_type_extended(pe.type) || pe.type == MBR_TYPE_GPT_PROT)
            continue;

        blkdev_t *p = part_create(disk, (uint32_t)(i + 1),
                                  pe.lba_start, pe.lba_len,
                                  pe.type, PART_TABLE_MBR);
        if (p)
            n++;
    }
    if (found) *found = n;
    return n;
}

// ── GPT parsing ───────────────────────────────────────────────────────────

static int part_parse_gpt(blkdev_t *disk, const uint8_t *lba1, int *found) {
    int n = 0;
    gpt_hdr_t hdr;
    memcpy(&hdr, lba1, sizeof(hdr));

    if (hdr.header_size < sizeof(gpt_hdr_t) || hdr.header_size > 512)
        return 0;
    if (hdr.num_entries == 0 || hdr.num_entries > 128)
        return 0;
    if (hdr.entry_size == 0 || hdr.entry_size > 512)
        return 0;
    if (hdr.entries_lba > 0xFFFFFFFFull || hdr.entries_lba == 0)
        return 0;

    uint32_t count = hdr.num_entries;
    if (count > BLKDEV_PARTS_PER_DISK)
        count = BLKDEV_PARTS_PER_DISK;

    uint8_t sec[512];
    uint8_t *entry_buf = (uint8_t *)kmalloc(count * hdr.entry_size);
    if (!entry_buf)
        return 0;
    memset(entry_buf, 0, count * hdr.entry_size);

    // Read the entry array (typically starts at LBA 2).
    uint32_t cur = (uint32_t)hdr.entries_lba;
    uint32_t need = count * hdr.entry_size;
    uint32_t have = 0;
    while (have < need && cur < disk->max_lba) {
        memset(sec, 0, 512);
        if (blkdev_read(disk, cur, sec) != 0)
            break;
        uint32_t take = need - have;
        if (take > 512) take = 512;
        memcpy(entry_buf + have, sec, take);
        have += take;
        cur++;
    }

    for (uint32_t i = 0; i < count; i++) {
        gpt_entry_t ge;
        memcpy(&ge, entry_buf + i * hdr.entry_size, sizeof(gpt_entry_t));
        if (all_zero16(ge.type_guid))
            continue;
        if (ge.first_lba == 0 || ge.last_lba < ge.first_lba)
            continue;
        uint64_t len = ge.last_lba - ge.first_lba + 1;
        blkdev_t *p = part_create(disk, i + 1, ge.first_lba, len,
                                  0 /* MBR type n/a */, PART_TABLE_GPT);
        if (p)
            n++;
    }

    kfree(entry_buf);
    if (found) *found = n;
    return n;
}

// ── scan entry point ──────────────────────────────────────────────────────

int part_scan_disk(blkdev_t *disk) {
    if (!disk || disk->parent != 0)
        return -1;
    if (disk->max_lba < 2)
        return 0;

    // Drop whatever the disk exposed before.
    part_drop_disk(disk);

    uint8_t lba0[512], lba1[512];
    memset(lba0, 0, 512);
    memset(lba1, 0, 512);
    if (blkdev_read(disk, 0, lba0) != 0) {
        printk("[part] "); printk(disk->name);
        printk(": read LBA0 failed, partition scan skipped\n");
        return -2;
    }
    if (blkdev_read(disk, 1, lba1) != 0) {
        printk("[part] "); printk(disk->name);
        printk(": read LBA1 failed, partition scan skipped\n");
        return -2;
    }

    disk->table = PART_TABLE_NONE;

    // GPT: header signature at LBA 1 (a GPT disk always has a protective MBR
    // at LBA 0, but the signature on LBA 1 is authoritative).
    if (mem_eq(lba1, GPT_SIGNATURE, 8) == 1) {
        int found = 0;
        part_parse_gpt(disk, lba1, &found);
        disk->table = PART_TABLE_GPT;
        printk("[part] "); printk(disk->name);
        printk(" GPT: ");
        {
            char b[16];
            snprintf(b, sizeof(b), "%d", found);
            printk(b);
        }
        printk(" partition(s)\n");
        return found;
    }

    // MBR: boot signature 0x55AA at the end of LBA 0.
    if (lba0[510] == 0x55 && lba0[511] == 0xAA) {
        int found = 0;
        part_parse_mbr(disk, lba0, &found);
        disk->table = PART_TABLE_MBR;
        printk("[part] "); printk(disk->name);
        printk(" MBR: ");
        {
            char b[16];
            snprintf(b, sizeof(b), "%d", found);
            printk(b);
        }
        printk(" partition(s)\n");
        return found;
    }

    printk("[part] "); printk(disk->name);
    printk(": no partition table (whole-disk volume)\n");
    return 0;
}

int part_scan_all(void) {
    int total = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        blkdev_t *d = blkdev_by_id((uint32_t)i);
        if (!d || d->parent != 0)
            continue;
        int n = part_scan_disk(d);
        if (n > 0)
            total += n;
    }
    return total;
}

int part_rescan(const char *name) {
    if (!name)
        return -1;
    blkdev_t *d = blkdev_find(name);
    if (!d || d->parent != 0)
        return -1;
    return part_scan_disk(d);
}

static void part_auto_scan(blkdev_t *disk) {
    (void)part_scan_disk(disk);
}

void part_probe_init(void) {
    blkdev_set_probe_hook(part_auto_scan);
    pr_info("  %-11s : auto-scan armed (partitions probed on disk register)\n",
            "part");
}
