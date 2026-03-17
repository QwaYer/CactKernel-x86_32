#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);

int pci_find_nvme(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn);

int search_pci(void);

//debug
void pci_dump_all(void);

#endif
