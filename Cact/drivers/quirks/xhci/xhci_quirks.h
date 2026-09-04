#ifndef CACT_QUIRKS_XHCI_H
#define CACT_QUIRKS_XHCI_H

#include <stdint.h>
#include "pci_enum.h"
#include "pci_driver.h"

/*
 * xHCI host controller quirks — one bit per workaround ("костыль").
 * No workaround behaviour is hardcoded in the driver outside these flags:
 * adding a broken controller means adding a row to the table in
 * xhci_quirks.c.
 */
#define XHCI_QUIRK_INTEL_HOST        (1u << 0)  /* Intel: extra settle after reset */
#define XHCI_QUIRK_SPURIOUS_REBOOT   (1u << 1)  /* Intel 300-series: spurious HSE must not reboot */

/* Look up the quirk bitmask for a PCI device (first table match wins). */
uint32_t xhci_quirks_for(pci_device_t *pdev);

#endif
