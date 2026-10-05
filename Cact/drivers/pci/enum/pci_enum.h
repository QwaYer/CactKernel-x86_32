#ifndef PCI_ENUM_H
#define PCI_ENUM_H

#include <stdint.h>
#include "pci.h"

// Scanner limits
#define MAX_PCI_DEVICES          128
#define PCI_MAX_DEV              32
#define PCI_MAX_FN               8

// Header type fields
#define PCI_HEADER_TYPE_NORMAL   0x00   // standard device
#define PCI_HEADER_TYPE_BRIDGE   0x01   // PCI-to-PCI bridge
#define PCI_HEADER_MULTIFUNCTION 0x80   // bit 7: multi-function device

// Bridge class codes
#define PCI_CLASS_BRIDGE         0x06
#define PCI_SUBCLASS_PCI_BRIDGE  0x04

// BAR type flags
#define PCI_BAR_IO               0x01   // I/O space BAR
#define PCI_BAR_MEM_TYPE_32      0x00   // 32-bit memory BAR

// Decoded Base Address Register descriptor.  base/size are 64-bit because
// memory BARs can be 64-bit typed: truncating to 32 bits mangles any BAR
// placed above 4 GiB and turns it into garbage (or aliased RAM).
typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t  is_io;      // 1 = I/O port, 0 = MMIO
} pci_bar_t;

// Enumerated PCI device — populated during bus scan
typedef struct pci_device {
    uint8_t   bus, dev, fn;

    uint16_t  vendor_id;
    uint16_t  device_id;
    uint8_t   class_code;
    uint8_t   subclass;
    uint8_t   prog_if;
    uint8_t   revision;
    uint8_t   header_type;

    pci_bar_t bars[6];   // up to 6 BARs (some may be unused)

    uint8_t   irq_line;
    uint8_t   irq_pin;
    uint8_t   drv_probe_state; // 0=pending,1=queued,2=ok,3=failed/no-driver

    int8_t    pcie_type;  // -1 = legacy PCI, 0..0xA = PCIe device type

    struct pci_device *next;   // global device list
} pci_device_t;

// Global device list — populated by pci_enumerate()
extern pci_device_t *pci_device_list;
extern uint32_t      pci_device_count;

// Walk the PCI bus hierarchy and populate pci_device_list
void          pci_enumerate(void);

// Search by class/subclass or vendor/device ID; returns NULL if not found
pci_device_t *pci_find_by_class(uint8_t class_code, uint8_t subclass);
pci_device_t *pci_find_by_id(uint16_t vendor_id, uint16_t device_id);

// N-th PCI function in pci_device_list (same 1-based index as [pci N] in /dev/modinfo)
pci_device_t *pci_device_by_index(int index_1based);

// Debug: dump all enumerated devices
void          pci_enum_dump(void);

#endif