#ifndef PCI_LOADER_H
#define PCI_LOADER_H

#include <stdint.h>

struct pci_driver;

int pci_load_module(const char *path, struct pci_driver *drv);

void pci_unload_module(struct pci_driver *drv);

#endif 