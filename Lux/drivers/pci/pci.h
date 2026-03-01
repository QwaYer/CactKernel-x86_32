#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define BM_CMD      0x00   
#define BM_STATUS   0x02  
#define BM_PRDT     0x04   

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);

uint16_t pci_find_ide_bm_base(void);

int search_pci(void);

//debug
void pci_dump_all(void);

#endif