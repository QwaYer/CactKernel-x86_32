#include "ata.h"
#include "buf.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6

#define ATA_REG_DATA      0   /* Data (16-bit)          */
#define ATA_REG_ERROR     1   /* Error / Features       */
#define ATA_REG_SECCOUNT  2   /* Sector Count           */
#define ATA_REG_LBA0      3   /* LBA bits  0-7          */
#define ATA_REG_LBA1      4   /* LBA bits  8-15         */
#define ATA_REG_LBA2      5   /* LBA bits 16-23         */
#define ATA_REG_DEVSEL    6   /* Device / LBA bits 24-27*/
#define ATA_REG_CMD       7   /* Command / Status       */


#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01


#define BM_BASE         0xC000 
#define BM_REG_CMD      0    
#define BM_REG_STATUS   2       
#define BM_REG_PRDT     4       

#define BM_CMD_START    0x01
#define BM_CMD_READ     0x08     

#define BM_ST_ACTIVE    0x01
#define BM_ST_ERROR     0x02
#define BM_ST_IRQ       0x04

static uint8_t active_drive = 0xE0;   // 0xE0 = master      

static void ata_delay(uint16_t base) {
    port_byte_in(base + ATA_REG_CMD);
    port_byte_in(base + ATA_REG_CMD);
    port_byte_in(base + ATA_REG_CMD);
    port_byte_in(base + ATA_REG_CMD);
}

static int ata_wait_bsy(uint16_t base) {
    for (int i = 0; i < 1000000; i++)
        if (!(port_byte_in(base + ATA_REG_CMD) & ATA_SR_BSY)) return 0;
    return -1;
}

static int ata_wait_drq(uint16_t base) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = port_byte_in(base + ATA_REG_CMD);
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DRQ) return  0;
    }
    return -1;
}

static int ata_wait_drdy(uint16_t base) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = port_byte_in(base + ATA_REG_CMD);
        if (s & ATA_SR_BSY)  continue;
        if (s & ATA_SR_DRDY) return 0;
    }
    return -1;
}

void ata_soft_reset(uint16_t base) {
    port_byte_out(ATA_PRIMARY_CTRL, 0x04);
    ata_delay(base);
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    ata_delay(base);
    ata_wait_bsy(base);
}

static int ata_identify(uint8_t drive_sel) {
    uint16_t base = ATA_PRIMARY_IO;
    port_byte_out(base + ATA_REG_DEVSEL, drive_sel);
    for (int d = 0; d < 15; d++) ata_delay(base);

    port_byte_out(base + ATA_REG_SECCOUNT, 0);
    port_byte_out(base + ATA_REG_LBA0,     0);
    port_byte_out(base + ATA_REG_LBA1,     0);
    port_byte_out(base + ATA_REG_LBA2,     0);
    port_byte_out(base + ATA_REG_CMD, 0xEC); /* IDENTIFY */
    ata_delay(base);

    uint8_t status = port_byte_in(base + ATA_REG_CMD);
    if (status == 0 || status == 0xFF) return 0;
    if (ata_wait_bsy(base) < 0)        return 0;

    if (port_byte_in(base + ATA_REG_LBA1) == 0x14 &&
        port_byte_in(base + ATA_REG_LBA2) == 0xEB)  return 0;

    if (ata_wait_drq(base) < 0) return 0;
    for (int i = 0; i < 256; i++) port_word_in(base + ATA_REG_DATA);
    return 1;
}

void ata_init(void) {
    binit();
    ata_soft_reset(ATA_PRIMARY_IO);

    if (ata_identify(0xB0)) {
        active_drive = 0xB0;
    } else if (ata_identify(0xE0)) {
        active_drive = 0xE0;
    } else {
        kprint("[ATA] No drive found!\n");
        return;
    }

    ata_soft_reset(ATA_PRIMARY_IO);
    kprint("[ATA] Drive detected, DMA ready.\n");
}

void ata_dma_start(uint16_t port, uint8_t slave,
                   uint32_t lba, int write,
                   struct prdt_entry *prdt)
{
    uint8_t drive = slave ? 0xF0 : 0xE0;

    port_byte_out(BM_BASE + BM_REG_CMD, 0x00);
    port_byte_out(BM_BASE + BM_REG_STATUS,
                  BM_ST_ERROR | BM_ST_IRQ);

    port_long_out(BM_BASE + BM_REG_PRDT, (uint32_t)prdt);

    uint8_t bm_cmd = write ? 0x00 : BM_CMD_READ;
    port_byte_out(BM_BASE + BM_REG_CMD, bm_cmd);

    if (ata_wait_drdy(port) < 0) {
        kprint("[ATA] ata_dma_start: drive not ready\n");
        return;
    }

    port_byte_out(port + ATA_REG_DEVSEL,
                  drive | 0x40 | ((lba >> 24) & 0x0F));
    ata_delay(port);

    port_byte_out(port + ATA_REG_ERROR,    0x00);
    port_byte_out(port + ATA_REG_SECCOUNT, 1);
    port_byte_out(port + ATA_REG_LBA0,  (uint8_t)(lba        & 0xFF));
    port_byte_out(port + ATA_REG_LBA1,  (uint8_t)((lba >>  8) & 0xFF));
    port_byte_out(port + ATA_REG_LBA2,  (uint8_t)((lba >> 16) & 0xFF));

    port_byte_out(port + ATA_REG_CMD, write ? 0xCA : 0xC8);

    port_byte_out(BM_BASE + BM_REG_CMD, bm_cmd | BM_CMD_START);
}

int ata_dma_stop(uint16_t port) {
    port_byte_out(BM_BASE + BM_REG_CMD, 0x00);

    uint8_t bm_status  = port_byte_in(BM_BASE + BM_REG_STATUS);
    uint8_t ata_status = port_byte_in(port + ATA_REG_CMD);

    port_byte_out(BM_BASE + BM_REG_STATUS,
                  BM_ST_ERROR | BM_ST_IRQ);

    if ((bm_status & BM_ST_ERROR) || (ata_status & ATA_SR_ERR))
        return -1;

    return 0;
}

void ata_irq_handler(void) {
    int error = ata_dma_stop(ATA_PRIMARY_IO);

    bio_irq_complete(error);
}

void ata_read_sector(uint16_t port, uint8_t slave,
                     uint32_t lba, uint8_t *buffer)
{
    uint16_t base  = port;
    uint8_t  drive = slave ? 0xF0 : 0xE0;

    port_byte_out(ATA_PRIMARY_CTRL, 0x02);

    if (ata_wait_drdy(base) < 0) goto fail;

    port_byte_out(base + ATA_REG_DEVSEL,
                  drive | 0x40 | ((lba >> 24) & 0x0F));
    ata_delay(base);

    port_byte_out(base + ATA_REG_ERROR,    0x00);
    port_byte_out(base + ATA_REG_SECCOUNT, 1);
    port_byte_out(base + ATA_REG_LBA0,  (uint8_t)(lba        & 0xFF));
    port_byte_out(base + ATA_REG_LBA1,  (uint8_t)((lba >>  8) & 0xFF));
    port_byte_out(base + ATA_REG_LBA2,  (uint8_t)((lba >> 16) & 0xFF));
    port_byte_out(base + ATA_REG_CMD, 0x20);   /* READ SECTORS */

    if (ata_wait_drq(base) < 0) goto fail;

    for (int i = 0; i < 256; i++) {
        uint16_t w = port_word_in(base + ATA_REG_DATA);
        buffer[i * 2]     = (uint8_t)(w & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)(w >> 8);
    }

    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    return;

fail:
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    for (int i = 0; i < 512; i++) buffer[i] = 0;
}

void ata_write_sector(uint16_t port, uint8_t slave,
                      uint32_t lba, uint8_t *buffer)
{
    uint16_t base  = port;
    uint8_t  drive = slave ? 0xF0 : 0xE0;

    port_byte_out(ATA_PRIMARY_CTRL, 0x02);

    if (ata_wait_drdy(base) < 0) {
        port_byte_out(ATA_PRIMARY_CTRL, 0x00);
        return;
    }

    port_byte_out(base + ATA_REG_DEVSEL,
                  drive | 0x40 | ((lba >> 24) & 0x0F));
    ata_delay(base);

    port_byte_out(base + ATA_REG_ERROR,    0x00);
    port_byte_out(base + ATA_REG_SECCOUNT, 1);
    port_byte_out(base + ATA_REG_LBA0,  (uint8_t)(lba        & 0xFF));
    port_byte_out(base + ATA_REG_LBA1,  (uint8_t)((lba >>  8) & 0xFF));
    port_byte_out(base + ATA_REG_LBA2,  (uint8_t)((lba >> 16) & 0xFF));
    port_byte_out(base + ATA_REG_CMD, 0x30);   /* WRITE SECTORS */

    if (ata_wait_drq(base) < 0) {
        port_byte_out(ATA_PRIMARY_CTRL, 0x00);
        return;
    }

    for (int i = 0; i < 256; i++) {
        uint16_t w = (uint16_t)buffer[i * 2] |
                     ((uint16_t)buffer[i * 2 + 1] << 8);
        port_word_out(base + ATA_REG_DATA, w);
    }

    port_byte_out(base + ATA_REG_CMD, 0xE7);   /* FLUSH CACHE */
    ata_wait_bsy(base);
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
}

int ata_read(struct vfs_node *node, uint32_t offset,
             uint32_t size, char *buffer)
{
    uint32_t blockno = offset / 512;
    uint32_t off_in  = offset % 512;

    struct buf *b = bread(0, blockno);
    if (!b) return 0;

    uint32_t avail = 512 - off_in;
    uint32_t n     = size < avail ? size : avail;
    for (uint32_t i = 0; i < n; i++)
        buffer[i] = (char)b->data[off_in + i];

    brelse(b);
    return (int)n;
}

int ata_write(struct vfs_node *node, uint32_t offset,
              uint32_t size, char *buffer)
{
    uint32_t blockno = offset / 512;
    uint32_t off_in  = offset % 512;

    struct buf *b = bread(0, blockno);
    if (!b) return 0;

    uint32_t avail = 512 - off_in;
    uint32_t n     = size < avail ? size : avail;
    for (uint32_t i = 0; i < n; i++)
        b->data[off_in + i] = (uint8_t)buffer[i];

    bwrite(b);
    brelse(b);
    return (int)n;
}

struct vfs_node *init_ata_device(void) {
    struct vfs_node *node = (struct vfs_node *)kalloc();
    if (!node) return 0;

    for (int i = 0; i < 128; i++) node->name[i] = 0;
    node->name[0] = 'h';
    node->name[1] = 'd';
    node->name[2] = 'a';

    node->type  = VFS_BLOCKDEVICE;
    node->read  = ata_read;
    node->write = ata_write;
    node->open  = 0;
    node->close = 0;
    node->ptr   = 0;

    return node;
}