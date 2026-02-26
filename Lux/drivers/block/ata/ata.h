#ifndef ATA_H
#define ATA_H

#include "vfs.h"
#include "buf.h"
#include <stdint.h>

void ata_init(void);

void ata_read_sector (uint16_t port, uint8_t slave, uint32_t lba, uint8_t *buf);
void ata_write_sector(uint16_t port, uint8_t slave, uint32_t lba, uint8_t *buf);

void ata_dma_start(uint16_t port, uint8_t slave, uint32_t lba,
                   int write, struct prdt_entry *prdt);

int  ata_dma_stop(uint16_t port);

void ata_irq_handler(void);

int              ata_read (struct vfs_node *node, uint32_t off, uint32_t sz, char *buf);
int              ata_write(struct vfs_node *node, uint32_t off, uint32_t sz, char *buf);
struct vfs_node *init_ata_device(void);

#endif 