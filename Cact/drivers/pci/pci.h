#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI Configuration Space I/O ports (Mechanism #1)
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Read/write 32-bit values from/to PCI configuration space.
// reg must be DWORD-aligned (lower 2 bits are masked off).
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);

// Return 0 if PCI Mechanism #1 is detected.
int search_pci(void);

// Debug: dump all PCI devices (implemented in pci_enum)
void pci_dump_all(void);

#endif