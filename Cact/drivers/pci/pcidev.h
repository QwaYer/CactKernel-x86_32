#ifndef PCIDEV_H
#define PCIDEV_H

#include <stdint.h>
#include "pci.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pcie.h"


/* Device and driver lists (owned by pcidev, accessible for iteration) */
extern pci_device_t *pci_device_list;
extern uint32_t      pci_device_count;
extern pci_driver_t *pcidev_driver_list;
extern uint32_t      pcidev_driver_count;

/* One-shot: PCIe ECAM init + legacy PCI probe + bus scan.
 * Call once after ACPI + APIC are up, before USB or any PCI driver init. */
void pcidev_init(void);

/* Probe all devices that were deferred during enumeration.
 * Call from the bootstrap thread once the scheduler is live. */
void pcidev_probe_all(void);

/* Internal: defer a device for later probing, or match immediately. */
void pcidev_defer_device(pci_device_t *dev);
int  pcidev_match_device(pci_device_t *dev);


uint32_t pcidev_cfg_read32(pci_device_t *dev, uint16_t reg);
void     pcidev_cfg_write32(pci_device_t *dev, uint16_t reg, uint32_t val);
uint8_t  pcidev_cfg_read8(pci_device_t *dev, uint16_t reg);
uint16_t pcidev_cfg_read16(pci_device_t *dev, uint16_t reg);
void     pcidev_cfg_write8(pci_device_t *dev, uint16_t reg, uint8_t val);
void     pcidev_cfg_write16(pci_device_t *dev, uint16_t reg, uint16_t val);

/* ── Driver registration (delegates to internal driver list) ── */

int pcidev_register_driver(pci_driver_t *drv);
int pcidev_unregister_driver(pci_driver_t *drv);


pci_device_t *pcidev_find_by_class(uint8_t class_code, uint8_t subclass);
pci_device_t *pcidev_find_by_id(uint16_t vendor_id, uint16_t device_id);
pci_device_t *pcidev_find_by_bdf(uint8_t bus, uint8_t dev, uint8_t fn);


void pcidev_enable_bus_master(pci_device_t *dev);
void pcidev_enable_mmio(pci_device_t *dev);
void pcidev_enable_io_space(pci_device_t *dev);
void pcidev_disable_intx(pci_device_t *dev);


int      pcidev_find_cap(pci_device_t *dev, uint8_t cap_id);
int      pcidev_get_pcie_type(pci_device_t *dev);
uint16_t pcidev_read_ext_cap(pci_device_t *dev, uint16_t cap_id,
                             uint16_t start);


void pcidev_dump(void);

#endif
