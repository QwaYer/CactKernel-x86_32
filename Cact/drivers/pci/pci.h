#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI Configuration Space I/O ports (Mechanism #1)
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Standard PCI capability IDs
#define PCI_CAP_ID_PWR      0x01    // PM
#define PCI_CAP_ID_AGP      0x02
#define PCI_CAP_ID_VPD      0x03
#define PCI_CAP_ID_SLOTID   0x04
#define PCI_CAP_ID_MSI      0x05
#define PCI_CAP_ID_CHSWP    0x06
#define PCI_CAP_ID_PCIX     0x07
#define PCI_CAP_ID_HT       0x08
#define PCI_CAP_ID_VNDR     0x09
#define PCI_CAP_ID_DBG      0x0A
#define PCI_CAP_ID_CCRC     0x0B
#define PCI_CAP_ID_HT_2     0x0C
#define PCI_CAP_ID_SSVID    0x0D    // sub-system vendor ID
#define PCI_CAP_ID_AGP_3    0x0E
#define PCI_CAP_ID_SECURE   0x0F
#define PCI_CAP_ID_EXP      0x10    // PCI Express
#define PCI_CAP_ID_MSIX     0x11

// PCI Express capability register offsets
#define PCI_EXP_CAP_ID      0x00
#define PCI_EXP_DEVCAP      0x04
#define PCI_EXP_DEVCTL      0x08
#define PCI_EXP_DEVSTA      0x0A
#define PCI_EXP_LNKCAP      0x0C
#define PCI_EXP_LNKCTL      0x10
#define PCI_EXP_LNKSTA      0x12
#define PCI_EXP_SLTCAP      0x14
#define PCI_EXP_SLTCTL      0x18
#define PCI_EXP_SLTSTA      0x1A
#define PCI_EXP_RTCTL       0x1C
#define PCI_EXP_RTCAP       0x1E
#define PCI_EXP_RTSTA       0x20

// PCI Express device type (from cap register bits [20:18] >> 4)
#define PCI_EXP_TYPE_ENDPOINT           0x0
#define PCI_EXP_TYPE_LEGACY_ENDPOINT    0x1
#define PCI_EXP_TYPE_ROOT_PORT          0x4
#define PCI_EXP_TYPE_UPSTREAM           0x5
#define PCI_EXP_TYPE_DOWNSTREAM         0x6
#define PCI_EXP_TYPE_PCI_BRIDGE         0x7
#define PCI_EXP_TYPE_RC_END             0x9
#define PCI_EXP_TYPE_RC_EC              0xA

// Extended capability list register (PCIe only, at offset 0x100)
#define PCI_EXT_CAP_BASE    0x100

// Read/write 32-bit values from/to PCI configuration space.
// reg must be DWORD-aligned (lower 2 bits are masked off).
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);

/* Aliases expected by driver code / docs (DWORD config access). */
uint32_t pci_read_config_long(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write_config_long(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                               uint32_t val);

/* Sets PCI command register bus-master bit (required for DMA). */
void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t fn);

// Return 0 if PCI Mechanism #1 is detected.
int search_pci(void);

// Debug: dump all PCI devices (implemented in pci_enum)
void pci_dump_all(void);

#endif