#ifndef ATA_H
#define ATA_H

#include "vfs.h"
#include <stdint.h>

void ata_init();

void ata_read_sector(uint16_t port, uint8_t slave, uint32_t lba, uint8_t* buffer);
void ata_write_sector(uint16_t port, uint8_t slave, uint32_t lba, uint8_t* buffer);

int ata_read(struct vfs_node* node, uint32_t offset, uint32_t size, char* buffer);
int ata_write(struct vfs_node* node, uint32_t offset, uint32_t size, char* buffer);
struct vfs_node* init_ata_device();

#endif