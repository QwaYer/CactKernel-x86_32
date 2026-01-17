#include "vfs.h"
#include "kernel.h"
#include "memory.h"

#define ATA_DATA        0x1F0
#define ATA_FEATURES    0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_SEL   0x1F6
#define ATA_COMMAND     0x1F7
#define ATA_STATUS      0x1F7

static void ata_wait_bsy() {
    while (port_byte_in(ATA_STATUS) & 0x80); 
}

static void ata_wait_drq() {
    while (!(port_byte_in(ATA_STATUS) & 0x08)); 
}

void ata_read_sector(unsigned int lba, unsigned short* buffer) {
    ata_wait_bsy();
    
    port_byte_out(ATA_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    port_byte_out(ATA_SECTOR_CNT, 1);
    port_byte_out(ATA_LBA_LOW, (unsigned char)lba);
    port_byte_out(ATA_LBA_MID, (unsigned char)(lba >> 8));
    port_byte_out(ATA_LBA_HIGH, (unsigned char)(lba >> 16));
    
    port_byte_out(ATA_COMMAND, 0x20); 

    ata_wait_drq();

    for (int i = 0; i < 256; i++) {
        buffer[i] = port_word_in(ATA_DATA);
    }
}

void ata_write_sector(unsigned int lba, unsigned short* buffer) {
    ata_wait_bsy();
    
    port_byte_out(ATA_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    port_byte_out(ATA_SECTOR_CNT, 1);
    port_byte_out(ATA_LBA_LOW, (unsigned char)lba);
    port_byte_out(ATA_LBA_MID, (unsigned char)(lba >> 8));
    port_byte_out(ATA_LBA_HIGH, (unsigned char)(lba >> 16));
    
    port_byte_out(ATA_COMMAND, 0x30);

    ata_wait_drq();

    for (int i = 0; i < 256; i++) {
        port_word_out(ATA_DATA, buffer[i]);
    }
}

int ata_read(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    unsigned int lba = offset / 512;
    ata_read_sector(lba, (unsigned short*)buffer);
    return size;
}

int ata_write(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    unsigned int lba = offset / 512;
    ata_write_sector(lba, (unsigned short*)buffer);
    return size;
}

struct vfs_node* init_ata_device() {
    struct vfs_node* node = (struct vfs_node*)kalloc();
    if (!node) return 0;

    for(int i=0; i<128; i++) node->name[i] = 0;
    node->name[0] = 'h'; node->name[1] = 'd'; node->name[2] = 'a';
    
    node->type = VFS_BLOCKDEVICE;
    node->read = ata_read;
    node->write = ata_write; 
    node->open = 0;
    node->close = 0;
    node->ptr = 0;

    return node;
}