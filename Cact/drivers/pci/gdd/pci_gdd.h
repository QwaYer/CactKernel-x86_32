#ifndef PCI_GDD_H
#define PCI_GDD_H

#include <stdint.h>
#include "pci_enum.h"

#define PCI_GDD_PI_ANY  0xFFu

/* Generic Driver Discovery: interactive class/subclass-based module load.
 * Parameters must match *dev* (validated inside). */
void pci_user_prompt_module(uint8_t cl, uint8_t sc, uint8_t pi, pci_device_t *dev);

/* Iterate all discovered PCI devices and prompt the user for driver modules. */
void pci_gdd_prompt_devices(void);

#endif
