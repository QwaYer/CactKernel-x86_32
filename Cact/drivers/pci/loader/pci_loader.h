#ifndef PCI_LOADER_H
#define PCI_LOADER_H

#include <stdint.h>

struct pci_driver;

// Load a relocatable ELF module, resolve "pci_driver_probe", wire into drv.
// Returns 0 on success, negative error code on failure.
int pci_load_module(const char *path, struct pci_driver *drv);

// Free module image and reset driver.probe to NULL.
void pci_unload_module(struct pci_driver *drv);

#endif