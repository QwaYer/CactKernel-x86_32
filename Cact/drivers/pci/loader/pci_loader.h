#ifndef PCI_LOADER_H
#define PCI_LOADER_H

#include <stdint.h>

struct pci_driver;

/* sys_module_load: pass (uint32_t)-1 for both vendor_id and device_id to use
 * cact_pci_* symbols inside the .cctk (see Virtio-net manifest). */
#define CACT_MODLOAD_ID_AUTO  0xFFFFFFFFu

// Read cact_pci_vendor_id, cact_pci_device_id(s), optional class/subclass from ET_REL on disk.
// Chooses device_id: first listed DID that appears on pci_device_list, else first in list.
// On success, *class_out / *subclass_out updated when symbols exist; else left unchanged.
// Callers should preset class/subclass to (uint8_t)PCI_ANY_ID (0xFF) for wildcard.
int pci_peek_module_manifest(const char *path,
                             uint16_t *vendor_out, uint16_t *device_out,
                             uint8_t *class_out, uint8_t *subclass_out);

// Load a relocatable ELF module, resolve "pci_driver_probe", wire into drv.
// Returns 0 on success, negative error code on failure.
int pci_load_module(const char *path, struct pci_driver *drv);

// Free module image and reset driver.probe to NULL.
void pci_unload_module(struct pci_driver *drv);

#endif