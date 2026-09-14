/*
 * Partition layer (MBR + GPT) for the Cact blkdev layer.
 *
 * Every whole-disk blkdev_t may carry up to BLKDEV_PARTS_PER_DISK partition
 * sub-devices.  Scanning reads the disk label through the parent device's own
 * I/O callbacks (blkdev_read), so it works for any storage driver and must
 * run in a task context (reads may sleep on controller IRQs).
 *
 * Each created device (whole disk and partition) is exposed as a single
 * /dev/<name> block node by vfsdev ("/dev/sda", "/dev/sda1" style).
 */

#include "blkdev.h"
#include "part.h"
#include "vfsdev.h"
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

// ── vfsdev block-node registration for partitions ─────────────────────────

static void part_devfs_add(blkdev_t *p) {
    if (p->devfs_registered)
        return;
    if (vfsdev_register_block_device(p) == 0)
        p->devfs_registered = 1;
}

static void part_devfs_remove(blkdev_t *p) {
    if (!p->devfs_registered)
        return;
    vfsdev_unregister_block_device(p);
    p->devfs_registered = 0;
}

int part_drop_disk(blkdev_t *disk) {
    if (!disk)
        return -1;
    // Unregister vfsdev block nodes first, then release the blkdev slots.
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
        pr_err("[part] %s: read LBA0 failed, partition scan skipped\n",
               disk->name);
        return -2;
    }
    if (blkdev_read(disk, 1, lba1) != 0) {
        pr_err("[part] %s: read LBA1 failed, partition scan skipped\n",
               disk->name);
        return -2;
    }

    disk->table = PART_TABLE_NONE;

    // GPT: header signature at LBA 1 (a GPT disk always has a protective MBR
    // at LBA 0, but the signature on LBA 1 is authoritative).
    if (mem_eq(lba1, GPT_SIGNATURE, 8) == 1) {
        int found = 0;
        part_parse_gpt(disk, lba1, &found);
        disk->table = PART_TABLE_GPT;
        pr_info("[part] %s GPT: %d partition(s)\n", disk->name, found);
        return found;
    }

    // MBR: boot signature 0x55AA at the end of LBA 0.
    if (lba0[510] == 0x55 && lba0[511] == 0xAA) {
        int found = 0;
        part_parse_mbr(disk, lba0, &found);
        disk->table = PART_TABLE_MBR;
        pr_info("[part] %s MBR: %d partition(s)\n", disk->name, found);
        return found;
    }

    pr_info("[part] %s: no partition table (whole-disk volume)\n", disk->name);
    return 0;
}

int part_scan_all(void) {
    int total = 0;
    int disks = 0;
    for (int i = 0; i < BLKDEV_SLOTS; i++) {
        blkdev_t *d = blkdev_by_id((uint32_t)i);
        if (!d || d->parent != 0)
            continue;
        disks++;
        int n = part_scan_disk(d);
        if (n > 0)
            total += n;
    }
    pr_info("  %-11s : %d disk(s) scanned, %d partition(s) exposed\n",
            "blkdev", disks, total);
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
    // Expose the whole disk itself under /dev first, then probe its label.
    vfsdev_register_block_device(disk);
    (void)part_scan_disk(disk);
}

void part_probe_init(void) {
    blkdev_set_probe_hook(part_auto_scan);
    pr_info("  %-11s : auto-scan armed (partitions probed on disk register)\n",
            "part");
}
