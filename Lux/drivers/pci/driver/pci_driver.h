#ifndef PCI_DRIVER_H
#define PCI_DRIVER_H

#include <stdint.h>
#include "pci_enum.h"

#define PCI_DRIVER_NAME_MAX   32
#define MAX_PCI_DRIVERS       32

#define PCI_ANY_ID  0xFFFF

typedef struct pci_driver {
    char     name[PCI_DRIVER_NAME_MAX];

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;

    int (*probe)(pci_device_t *dev);

    void (*remove)(pci_device_t *dev);

    const char *module_path;

    void *priv;

    struct pci_driver *next;
} pci_driver_t;

int  pci_register_driver  (pci_driver_t *drv);
int  pci_unregister_driver(pci_driver_t *drv);

void pci_driver_match(pci_device_t *dev);

//Debug
void pci_driver_dump(void);

#endif 