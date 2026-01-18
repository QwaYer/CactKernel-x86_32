#include "vfs.h"
#include "kernel.h"
#include "memory.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_SECONDARY_IO    0x170

void ata_soft_reset(unsigned short io_base) {
    unsigned short control_port = io_base + 0x206;
    port_byte_out(control_port, 0x04);
    
    volatile int delay = 50000;
    while (delay-- > 0);
    
    port_byte_out(control_port, 0x00);
    
    delay = 20000;
    while (delay-- > 0);
}

int ata_identify_drive(unsigned short io_base, unsigned char slave) {
    unsigned short cmd_port = io_base + 7;
    unsigned short status_port = io_base + 7;
    unsigned short error_port = io_base + 1;
    unsigned short select_port = io_base + 6;
    
    unsigned char drive_sel = slave ? 0xB0 : 0xA0;
    port_byte_out(select_port, drive_sel);
    
    volatile int delay = 1000;
    while (delay-- > 0);
    
    port_byte_out(cmd_port, 0xEC);
    
    delay = 1000;
    while (delay-- > 0);
    
    unsigned char status = port_byte_in(status_port);
    
    if (status == 0 || status == 0xFF) {
        return 0;
    }
    
    int timeout = 1000000;
    while (timeout-- > 0) {
        status = port_byte_in(status_port);
        if (status & 0x08) {
            for (int i = 0; i < 256; i++) {
                port_word_in(io_base);
            }
            return 1;
        }
        if (status & 0x01) {
            return 0;
        }
    }
    
    return 0;
}

void ata_init() {
    ata_soft_reset(ATA_PRIMARY_IO);
    ata_identify_drive(ATA_PRIMARY_IO, 0);
}

void ata_read_sector(unsigned int lba, unsigned char* buffer) {
    unsigned short io_base = ATA_PRIMARY_IO;
    
    unsigned short cmd_port = io_base + 7;
    unsigned short data_port = io_base;
    unsigned short error_port = io_base + 1;
    unsigned short sector_cnt_port = io_base + 2;
    unsigned short lba_low_port = io_base + 3;
    unsigned short lba_mid_port = io_base + 4;
    unsigned short lba_high_port = io_base + 5;
    unsigned short select_port = io_base + 6;
    unsigned short status_port = io_base + 7;
    
    for (int i = 0; i < 512; i++) {
        buffer[i] = 0;
    }
    
    volatile int timeout = 1000000;
    while ((port_byte_in(status_port) & 0x80) && timeout-- > 0);
    
    port_byte_out(select_port, 0xE0);
    
    volatile int delay = 10000;
    while (delay-- > 0);
    
    port_byte_out(sector_cnt_port, 1);
    port_byte_out(lba_low_port, (unsigned char)(lba & 0xFF));
    port_byte_out(lba_mid_port, (unsigned char)((lba >> 8) & 0xFF));
    port_byte_out(lba_high_port, (unsigned char)((lba >> 16) & 0xFF));
    
    unsigned char drive_sel = 0xE0 | ((lba >> 24) & 0x0F);
    port_byte_out(select_port, drive_sel);
    
    port_byte_out(cmd_port, 0x20);
    
    delay = 10000;
    while (delay-- > 0);
    
    timeout = 10000000;
    while (timeout-- > 0) {
        unsigned char status = port_byte_in(status_port);
        
        if (status & 0x01) {
            return;
        }
        
        if (status & 0x08) {
            break;
        }
    }
    
    for (int i = 0; i < 256; i++) {
        unsigned short word = port_word_in(data_port);
        buffer[i*2] = (unsigned char)(word & 0xFF);
        buffer[i*2+1] = (unsigned char)((word >> 8) & 0xFF);
    }
}

void ata_write_sector(unsigned int lba, unsigned char* buffer) {
    unsigned short io_base = ATA_PRIMARY_IO;
    
    unsigned short cmd_port = io_base + 7;
    unsigned short data_port = io_base;
    unsigned short sector_cnt_port = io_base + 2;
    unsigned short lba_low_port = io_base + 3;
    unsigned short lba_mid_port = io_base + 4;
    unsigned short lba_high_port = io_base + 5;
    unsigned short select_port = io_base + 6;
    unsigned short status_port = io_base + 7;
    
    volatile int timeout = 1000000;
    while ((port_byte_in(status_port) & 0x80) && timeout-- > 0);
    
    port_byte_out(select_port, 0xE0);
    
    volatile int delay = 10000;
    while (delay-- > 0);
    
    port_byte_out(sector_cnt_port, 1);
    port_byte_out(lba_low_port, (unsigned char)(lba & 0xFF));
    port_byte_out(lba_mid_port, (unsigned char)((lba >> 8) & 0xFF));
    port_byte_out(lba_high_port, (unsigned char)((lba >> 16) & 0xFF));
    
    unsigned char drive_sel = 0xE0 | ((lba >> 24) & 0x0F);
    port_byte_out(select_port, drive_sel);
    
    port_byte_out(cmd_port, 0x30);
    
    delay = 10000;
    while (delay-- > 0);
    
    timeout = 10000000;
    while (timeout-- > 0) {
        unsigned char status = port_byte_in(status_port);
        
        if (status & 0x01) {
            return;
        }
        
        if (status & 0x08) {
            break;
        }
    }
    
    for (int i = 0; i < 256; i++) {
        unsigned short word = (unsigned short)(buffer[i*2]) | ((unsigned short)(buffer[i*2+1]) << 8);
        port_word_out(data_port, word);
    }
}

int ata_read(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    unsigned int lba = offset / 512;
    ata_read_sector(lba, (unsigned char*)buffer);
    return size;
}

int ata_write(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    unsigned int lba = offset / 512;
    ata_write_sector(lba, (unsigned char*)buffer);
    return size;
}

struct vfs_node* init_ata_device() {
    struct vfs_node* node = (struct vfs_node*)kalloc();
    if (!node) {
        return 0;
    }

    for(int i = 0; i < 128; i++) node->name[i] = 0;
    node->name[0] = 'h'; 
    node->name[1] = 'd'; 
    node->name[2] = 'a';
    
    node->type = VFS_BLOCKDEVICE;
    node->read = ata_read;
    node->write = ata_write; 
    node->open = 0;
    node->close = 0;
    node->ptr = 0;

    return node;
}