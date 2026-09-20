#ifndef CACT_MSI_H
#define CACT_MSI_H

#include <stdint.h>
#include "pci_enum.h"

/*
 * Umbrella header for the interrupt subsystem.  The pieces are split by
 * mechanism so the layers stay separable:
 *
 *   msidev.h — vector pool, IDT stubs, dispatch, msidev_register()
 *   msix.h   — MSI-X capability (ID 0x11)
 *   msi.h    — MSI capability (ID 0x05), declared below
 */
#include "msidev.h"
#include "msix.h"

/*
 * MSI (capability ID 0x05) — the single-message predecessor of MSI-X, for
 * controllers that expose MSI but no MSI-X (e.g. Intel PCH xHCI).  An MSI
 * interrupt is just a LAPIC message carrying the vector, so delivery reuses
 * the shared pool from msidev.h.
 *
 * Unlike the MSI-X table, the MSI message registers live in configuration
 * space below 0x100, so the suspend/resume configuration snapshot already
 * restores them — there is no MSI counterpart to msix_restore().
 */
int pci_msi_support(pci_device_t *dev);
int pci_msi_enable(pci_device_t *dev, int vector);

#endif
