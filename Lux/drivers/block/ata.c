#include "ata.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

static void ata_delay(unsigned short io_base) {
    port_byte_in(io_base + 7);
    port_byte_in(io_base + 7);
    port_byte_in(io_base + 7);
    port_byte_in(io_base + 7);
}

static int ata_wait_ready(unsigned short io_base) {
    int timeout = 100000;
    unsigned char status;
    while (timeout-- > 0) {
        status = port_byte_in(io_base + 7);
        if (!(status & 0x80)) return (status & 0x01) ? -1 : 0;
    }
    return -1;
}

static int ata_wait_drq(unsigned short io_base) {
    int timeout = 100000;
    unsigned char status;
    while (timeout-- > 0) {
        status = port_byte_in(io_base + 7);
        if (status & 0x01) return -1;   
        if (status & 0x08) return 0;  
    }
    return -1;
}

void ata_soft_reset(unsigned short io_base) {
    port_byte_out(ATA_PRIMARY_CTRL, 0x04);  
    ata_delay(io_base);
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);  
    ata_delay(io_base);
    ata_wait_ready(io_base);
}

void ata_init() {
    ata_soft_reset(ATA_PRIMARY_IO);

    port_byte_out(ATA_PRIMARY_IO + 6, 0xA0);
    ata_delay(ATA_PRIMARY_IO);

    port_byte_out(ATA_PRIMARY_IO + 7, 0xEC);
    ata_delay(ATA_PRIMARY_IO);

    unsigned char status = port_byte_in(ATA_PRIMARY_IO + 7);
    if (status == 0 || status == 0xFF) return;

    if (ata_wait_drq(ATA_PRIMARY_IO) == 0) {
        for (int i = 0; i < 256; i++)
            port_word_in(ATA_PRIMARY_IO);
    }
}

void ata_read_sector(unsigned int lba, unsigned char* buffer) {
    unsigned short base = ATA_PRIMARY_IO;
    if (ata_wait_ready(base) < 0) {
        goto fail;
    }

    port_byte_out(base + 6, 0xE0 | ((lba >> 24) & 0x0F));  
    ata_delay(base);

    port_byte_out(base + 2, 1);                        
    port_byte_out(base + 3, (unsigned char)(lba & 0xFF));
    port_byte_out(base + 4, (unsigned char)((lba >> 8) & 0xFF));
    port_byte_out(base + 5, (unsigned char)((lba >> 16) & 0xFF));
    port_byte_out(base + 7, 0x20);                          

    ata_delay(base);

    if (ata_wait_drq(base) < 0) {
        goto fail;
    }

    for (int i = 0; i < 256; i++) {
        unsigned short word = port_word_in(base);
        buffer[i * 2]     = (unsigned char)(word & 0xFF);
        buffer[i * 2 + 1] = (unsigned char)((word >> 8) & 0xFF);
    }
    return;

fail:
    for (int i = 0; i < 512; i++) buffer[i] = 0;
}

void ata_write_sector(unsigned int lba, unsigned char* buffer) {
    unsigned short base = ATA_PRIMARY_IO;

    if (ata_wait_ready(base) < 0) return;

    port_byte_out(base + 6, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay(base);

    port_byte_out(base + 2, 1);
    port_byte_out(base + 3, (unsigned char)(lba & 0xFF));
    port_byte_out(base + 4, (unsigned char)((lba >> 8) & 0xFF));
    port_byte_out(base + 5, (unsigned char)((lba >> 16) & 0xFF));
    port_byte_out(base + 7, 0x30);                       

    if (ata_wait_drq(base) < 0) return;

    for (int i = 0; i < 256; i++) {
        unsigned short word = (unsigned short)buffer[i * 2] |
                              ((unsigned short)buffer[i * 2 + 1] << 8);
        port_word_out(base, word);
    }

    port_byte_out(base + 7, 0xE7);
    ata_wait_ready(base);
}

int ata_read(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    ata_read_sector(offset / 512, (unsigned char*)buffer);
    return size;
}

int ata_write(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    ata_write_sector(offset / 512, (unsigned char*)buffer);
    return size;
}

struct vfs_node* init_ata_device() {
    struct vfs_node* node = (struct vfs_node*)kalloc();
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