#include "ata.h"
#include "pci.h"
#include "buf.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

static uint16_t bm_base = 0;

#define BM_CMD_PRIMARY    (bm_base + 0x00)
#define BM_STATUS_PRIMARY (bm_base + 0x02)
#define BM_PRDT_PRIMARY   (bm_base + 0x04)

static struct buf* disk_queue      = 0;
static struct buf* disk_queue_tail = 0;

static void ata_delay(void) {
    port_byte_in(ATA_PRIMARY_IO + 7);
    port_byte_in(ATA_PRIMARY_IO + 7);
    port_byte_in(ATA_PRIMARY_IO + 7);
    port_byte_in(ATA_PRIMARY_IO + 7);
}

static int ata_wait_bsy(void) {
    for (int i = 0; i < 100000; i++)
        if (!(port_byte_in(ATA_PRIMARY_IO + 7) & 0x80)) return 0;
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = port_byte_in(ATA_PRIMARY_IO + 7);
        if (s & 0x01) return -1;
        if (s & 0x08) return  0;
    }
    return -1;
}

void ata_soft_reset(void) {
    port_byte_out(ATA_PRIMARY_CTRL, 0x04);
    ata_delay();
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    ata_delay();
    ata_wait_bsy();
}

static void dma_start(struct buf* b) {
    int is_write = (b->flags & B_DIRTY);

    port_byte_out(BM_CMD_PRIMARY, 0x00);

    port_byte_out(BM_STATUS_PRIMARY,
        port_byte_in(BM_STATUS_PRIMARY) | 0x06);

    b->prdt[0].phys_addr  = (uint32_t)b->data;
    b->prdt[0].byte_count = 512;
    b->prdt[0].flags      = PRDT_EOT;
    port_dword_out(BM_PRDT_PRIMARY, (uint32_t)b->prdt);

    uint32_t lba  = b->blockno;
    uint8_t  slave = (uint8_t)(b->dev & 1);
    uint8_t  drv   = slave ? 0xF0 : 0xE0;

    port_byte_out(ATA_PRIMARY_CTRL, 0x00);  
    port_byte_out(ATA_PRIMARY_IO + 6, drv | 0x40 | ((lba >> 24) & 0x0F));
    ata_delay();

    if (ata_wait_bsy() < 0) {
        kprint("[ATA] dma_start: drive not ready\n");
        bio_irq_complete(1);
        return;
    }

    port_byte_out(ATA_PRIMARY_IO + 1, 0x00);
    port_byte_out(ATA_PRIMARY_IO + 2, 1);
    port_byte_out(ATA_PRIMARY_IO + 3, (uint8_t)( lba        & 0xFF));
    port_byte_out(ATA_PRIMARY_IO + 4, (uint8_t)((lba >>  8) & 0xFF));
    port_byte_out(ATA_PRIMARY_IO + 5, (uint8_t)((lba >> 16) & 0xFF));
    port_byte_out(ATA_PRIMARY_IO + 7, is_write ? 0xCA : 0xC8);

    port_byte_out(BM_CMD_PRIMARY, is_write ? 0x01 : 0x09);
}

void bio_enqueue_sync(struct buf* b) {
    b->qnext = 0;
    b->flags |= B_QUEUED;
    if (!disk_queue) {
        disk_queue = disk_queue_tail = b;
        dma_start(b);
    } else {
        disk_queue_tail->qnext = b;
        disk_queue_tail = b;
    }
}

void ata_irq_handler(void) {
    uint8_t bm_st = port_byte_in(BM_STATUS_PRIMARY);
    port_byte_out(BM_CMD_PRIMARY, 0x00);           
    port_byte_out(BM_STATUS_PRIMARY, bm_st | 0x06);  
    port_byte_in(ATA_PRIMARY_IO + 7);                
    bio_irq_complete((bm_st & 0x02) ? 1 : 0);
}

void bio_irq_complete(int error) {
    if (!disk_queue) return;

    struct buf* done = disk_queue;
    disk_queue = done->qnext;
    if (!disk_queue) disk_queue_tail = 0;

    done->flags &= ~B_QUEUED;
    if (error) {
        done->flags |= B_ERROR;
    } else {
        done->flags |= B_VALID;
        done->flags &= ~B_DIRTY;
    }

    if (done->callback) {
        done->callback(done, error);
    }

    if (disk_queue)
        dma_start(disk_queue);
}

static int ata_identify(uint8_t drive_sel) {
    port_byte_out(ATA_PRIMARY_IO + 6, drive_sel);
    for (int d = 0; d < 15; d++) ata_delay();
    port_byte_out(ATA_PRIMARY_IO + 2, 0);
    port_byte_out(ATA_PRIMARY_IO + 3, 0);
    port_byte_out(ATA_PRIMARY_IO + 4, 0);
    port_byte_out(ATA_PRIMARY_IO + 5, 0);
    port_byte_out(ATA_PRIMARY_IO + 7, 0xEC);
    ata_delay();
    uint8_t st = port_byte_in(ATA_PRIMARY_IO + 7);
    if (!st || st == 0xFF) return 0;
    if (ata_wait_bsy() < 0) return 0;
    if (port_byte_in(ATA_PRIMARY_IO + 4) == 0x14 &&
        port_byte_in(ATA_PRIMARY_IO + 5) == 0xEB) return 0;
    if (ata_wait_drq() < 0) return 0;
    for (int i = 0; i < 256; i++) port_word_in(ATA_PRIMARY_IO);
    return 1;
}

void ata_init(void) {
    binit();
    bm_base = pci_find_ide_bm_base();
    if (!bm_base)
        kprint("[ATA] WARNING: No Bus Master IDE, DMA unavailable\n");

    ata_soft_reset();

    for (int i = 0; i < 1000000; i++)
        if (!(port_byte_in(ATA_PRIMARY_IO + 7) & 0x80)) break;

    if      (ata_identify(0xE0));
    else if (ata_identify(0xB0));
    else {
        kprint("[ATA] Drive ready (PIO mode).\n");
    }
}


void ata_read_sector(uint16_t port, uint8_t slave, uint32_t lba, uint8_t* buffer) {
    uint8_t drv = slave ? 0xF0 : 0xE0;
    port_byte_out(ATA_PRIMARY_CTRL, 0x02);
    port_byte_out(port + 6, drv | 0x40 | ((lba >> 24) & 0x0F));
    ata_delay();
    if (ata_wait_bsy() < 0) goto fail;
    port_byte_out(port + 1, 0x00);
    port_byte_out(port + 2, 1);
    port_byte_out(port + 3, (uint8_t)( lba        & 0xFF));
    port_byte_out(port + 4, (uint8_t)((lba >>  8) & 0xFF));
    port_byte_out(port + 5, (uint8_t)((lba >> 16) & 0xFF));
    port_byte_out(port + 7, 0x20);
    if (ata_wait_drq() < 0) goto fail;
    for (int i = 0; i < 256; i++) {
        uint16_t w = port_word_in(port);
        buffer[i*2]   = (uint8_t)(w & 0xFF);
        buffer[i*2+1] = (uint8_t)(w >> 8);
    }
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    return;
fail:
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    memory_set(buffer, 0, 512);
}

void ata_write_sector(uint16_t port, uint8_t slave, uint32_t lba, uint8_t* buffer) {
    uint8_t drv = slave ? 0xF0 : 0xE0;
    port_byte_out(ATA_PRIMARY_CTRL, 0x02);
    port_byte_out(port + 6, drv | 0x40 | ((lba >> 24) & 0x0F));
    ata_delay();
    if (ata_wait_bsy() < 0) { port_byte_out(ATA_PRIMARY_CTRL, 0x00); return; }
    port_byte_out(port + 1, 0x00);
    port_byte_out(port + 2, 1);
    port_byte_out(port + 3, (uint8_t)( lba        & 0xFF));
    port_byte_out(port + 4, (uint8_t)((lba >>  8) & 0xFF));
    port_byte_out(port + 5, (uint8_t)((lba >> 16) & 0xFF));
    port_byte_out(port + 7, 0x30);
    if (ata_wait_drq() < 0) { port_byte_out(ATA_PRIMARY_CTRL, 0x00); return; }
    for (int i = 0; i < 256; i++) {
        uint16_t w = (uint16_t)buffer[i*2] | ((uint16_t)buffer[i*2+1] << 8);
        port_word_out(port, w);
    }
    port_byte_out(port + 7, 0xE7);
    ata_wait_bsy();
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
}


int ata_read(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    struct buf* b = bread(0, offset / 512);
    if (!b) return 0;
    for (unsigned int i = 0; i < size; i++) buffer[i] = b->data[i];
    brelse(b);
    return size;
}

int ata_write(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    struct buf* b = bread(0, offset / 512);
    if (!b) return 0;
    for (unsigned int i = 0; i < size; i++) b->data[i] = buffer[i];
    bwrite(b);
    brelse(b);
    return size;
}

struct vfs_node* init_ata_device(void) {
    struct vfs_node* node = (struct vfs_node*)kalloc();
    if (!node) return 0;
    memory_set(node->name, 0, 128);
    node->name[0] = 'h'; node->name[1] = 'd'; node->name[2] = 'a';
    node->type  = VFS_BLOCKDEVICE;
    node->read  = ata_read;
    node->write = ata_write;
    return node;
}