#ifndef PCI_ENUM_H
#define PCI_ENUM_H

#include <stdint.h>
#include "pci.h"

#define MAX_PCI_DEVICES          64
#define PCI_MAX_DEV              32
#define PCI_MAX_FN               8


#define PCI_HEADER_TYPE_NORMAL   0x00
#define PCI_HEADER_TYPE_BRIDGE   0x01   
#define PCI_HEADER_MULTIFUNCTION 0x80


#define PCI_CLASS_BRIDGE         0x06
#define PCI_SUBCLASS_PCI_BRIDGE  0x04


#define PCI_BAR_IO               0x01
#define PCI_BAR_MEM_TYPE_32      0x00

typedef struct {
    uint32_t base;
    uint32_t size;
    uint8_t  is_io;   
} pci_bar_t;

typedef struct pci_device {
    uint8_t   bus, dev, fn;

    uint16_t  vendor_id;
    uint16_t  device_id;
    uint8_t   class_code;
    uint8_t   subclass;
    uint8_t   prog_if;
    uint8_t   revision;
    uint8_t   header_type;

    pci_bar_t bars[6];   

    uint8_t   irq_line;
    uint8_t   irq_pin;

    struct pci_device *next;  
} pci_device_t;

extern pci_device_t *pci_device_list;
extern uint32_t      pci_device_count;

void          pci_enumerate(void);
pci_device_t *pci_find_by_class(uint8_t class_code, uint8_t subclass);
pci_device_t *pci_find_by_id(uint16_t vendor_id, uint16_t device_id);
void          pci_enum_dump(void);

#endif 