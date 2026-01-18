#ifndef ATA_H
#define ATA_H

#include "vfs.h"

void ata_init();

void ata_read_sector(unsigned int lba, unsigned char* buffer);
void ata_write_sector(unsigned int lba, unsigned char* buffer);

int ata_read(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
int ata_write(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
struct vfs_node* init_ata_device();

#endif