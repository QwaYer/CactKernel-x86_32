#ifndef PCI_DRIVER_H
#define PCI_DRIVER_H

#include <stdint.h>
#include "pci_enum.h"

// Limits
#define PCI_DRIVER_NAME_MAX   32
#define MAX_PCI_DRIVERS       32

// Wildcard for driver matching — matches any vendor/device/class/subclass
#define PCI_ANY_ID  0xFFFF

// PCI driver descriptor — one per supported device class
typedef struct pci_driver {
    char     name[PCI_DRIVER_NAME_MAX];

    uint16_t vendor_id;        // PCI_ANY_ID = wildcard
    uint16_t device_id;        // PCI_ANY_ID = wildcard
    uint8_t  class_code;       // PCI_ANY_ID = wildcard
    uint8_t  subclass;         // PCI_ANY_ID = wildcard

    int (*probe)(pci_device_t *dev);    // called when device matched
    void (*remove)(pci_device_t *dev);  // optional teardown

    const char *module_path;            // for lazy-loading .cctk modules

    void *priv;                         // driver-private data (e.g. module memory)

    struct pci_driver *next;            // linked list
} pci_driver_t;

// Register/unregister a driver in the global driver list.
int  pci_register_driver  (pci_driver_t *drv);
int  pci_unregister_driver(pci_driver_t *drv);

// Walk driver list and probe the first matching driver for a device.
void pci_driver_match(pci_device_t *dev);

// Debug: print all registered drivers.
void pci_driver_dump(void);

#endif