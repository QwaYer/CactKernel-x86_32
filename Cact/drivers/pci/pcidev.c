#include "pcidev.h"
#include "kernel.h"
#include "klib.h"
#include "pci_gdd.h"


pci_device_t *pci_device_list  = NULL;
uint32_t      pci_device_count = 0;


pci_driver_t *pcidev_driver_list  = NULL;
uint32_t      pcidev_driver_count = 0;

#define PCI_DEFER_MAX  MAX_PCI_DEVICES
static pci_device_t *deferred_devs[PCI_DEFER_MAX];
static uint32_t      deferred_count = 0;


static inline uint32_t bdf_addr(pci_device_t *d)
{
    return ((uint32_t)d->bus << 16) | ((uint32_t)d->dev << 11)
         | ((uint32_t)d->fn << 8);
}

uint32_t pcidev_cfg_read32(pci_device_t *dev, uint16_t reg)
{
    if (!dev) return 0xFFFFFFFF;
    if (pcie_is_available())
        return pcie_read32(dev->bus, dev->dev, dev->fn, reg);
    return pci_read_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)reg);
}

void pcidev_cfg_write32(pci_device_t *dev, uint16_t reg, uint32_t val)
{
    if (!dev) return;
    if (pcie_is_available()) {
        pcie_write32(dev->bus, dev->dev, dev->fn, reg, val);
        return;
    }
    pci_write_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)reg, val);
}

uint8_t pcidev_cfg_read8(pci_device_t *dev, uint16_t reg)
{
    if (!dev) return 0xFF;
    if (pcie_is_available())
        return pcie_read8(dev->bus, dev->dev, dev->fn, reg);
    uint32_t v = pci_read_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)(reg & ~3));
    return (uint8_t)(v >> ((reg & 3) * 8));
}

uint16_t pcidev_cfg_read16(pci_device_t *dev, uint16_t reg)
{
    if (!dev) return 0xFFFF;
    if (pcie_is_available())
        return pcie_read16(dev->bus, dev->dev, dev->fn, reg);
    uint32_t v = pci_read_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)(reg & ~3));
    return (uint16_t)(v >> ((reg & 2) * 8));
}

void pcidev_cfg_write8(pci_device_t *dev, uint16_t reg, uint8_t val)
{
    if (!dev) return;
    if (pcie_is_available()) {
        pcie_write8(dev->bus, dev->dev, dev->fn, reg, val);
        return;
    }
    uint32_t v = pci_read_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)(reg & ~3));
    uint32_t shift = (reg & 3) * 8;
    v = (v & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    pci_write_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)(reg & ~3), v);
}

void pcidev_cfg_write16(pci_device_t *dev, uint16_t reg, uint16_t val)
{
    if (!dev) return;
    if (pcie_is_available()) {
        pcie_write16(dev->bus, dev->dev, dev->fn, reg, val);
        return;
    }
    uint32_t v = pci_read_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)(reg & ~3));
    uint32_t shift = (reg & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_write_config_dword(dev->bus, dev->dev, dev->fn, (uint8_t)(reg & ~3), v);
}


static int driver_matches(const pci_driver_t *drv, const pci_device_t *dev)
{
    if (drv->vendor_id  != PCI_ANY_ID          && drv->vendor_id  != dev->vendor_id)  return 0;
    if (drv->device_id  != PCI_ANY_ID          && drv->device_id  != dev->device_id)  return 0;
    if (drv->class_code != (uint8_t)PCI_ANY_ID && drv->class_code != dev->class_code) return 0;
    if (drv->subclass   != (uint8_t)PCI_ANY_ID && drv->subclass   != dev->subclass)   return 0;
    return 1;
}

int pcidev_register_driver(pci_driver_t *drv)
{
    if (!drv || (!drv->probe && !drv->module_path)) return -1;
    if (pcidev_driver_count >= MAX_PCI_DRIVERS) {
        klog(LOG_WARN, "pcidev: driver table full");
        return -1;
    }
    for (pci_driver_t *d = pcidev_driver_list; d; d = d->next)
        if (streq(d->name, drv->name)) {
            klog(LOG_WARN, "pcidev: duplicate driver name");
            return -1;
        }
    drv->next   = pcidev_driver_list;
    pcidev_driver_list = drv;
    pcidev_driver_count++;
    return 0;
}

int pcidev_unregister_driver(pci_driver_t *drv)
{
    pci_driver_t **pp = &pcidev_driver_list;
    while (*pp) {
        if (*pp == drv) {
            *pp = drv->next;
            pcidev_driver_count--;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}


#include "pci_loader.h"

int pcidev_match_device(pci_device_t *dev)
{
    for (pci_driver_t *drv = pcidev_driver_list; drv; drv = drv->next) {
        if (!driver_matches(drv, dev)) continue;

        if (drv->module_path && !drv->probe) {
            if (pci_load_module(drv->module_path, drv) != 0) {
                klog(LOG_WARN, "pcidev: module load failed");
                continue;
            }
        }

        if (!drv->probe) return -1;

        klog(LOG_OK, "pcidev: driver attached");
        if (drv->probe(dev) != 0) return -1;
        return 1;
    }
    return 0;
}


pci_device_t *pcidev_find_by_class(uint8_t class_code, uint8_t subclass)
{
    for (pci_device_t *d = pci_device_list; d; d = d->next)
        if (d->class_code == class_code && d->subclass == subclass)
            return d;
    return NULL;
}

pci_device_t *pcidev_find_by_id(uint16_t vendor_id, uint16_t device_id)
{
    for (pci_device_t *d = pci_device_list; d; d = d->next)
        if (d->vendor_id == vendor_id && d->device_id == device_id)
            return d;
    return NULL;
}

pci_device_t *pcidev_find_by_bdf(uint8_t bus, uint8_t dev, uint8_t fn)
{
    for (pci_device_t *d = pci_device_list; d; d = d->next)
        if (d->bus == bus && d->dev == dev && d->fn == fn)
            return d;
    return NULL;
}


#define PCI_CMD_REG      0x04
#define PCI_CMD_BUS_MASTER  (1u << 2)
#define PCI_CMD_MEM_SPACE   (1u << 1)
#define PCI_CMD_IO_SPACE    (1u << 0)
#define PCI_CMD_INTX_DISABLE (1u << 10)

void pcidev_enable_bus_master(pci_device_t *dev)
{
    uint32_t cmd = pcidev_cfg_read32(dev, PCI_CMD_REG);
    pcidev_cfg_write32(dev, PCI_CMD_REG, cmd | PCI_CMD_BUS_MASTER);
}

void pcidev_enable_mmio(pci_device_t *dev)
{
    uint32_t cmd = pcidev_cfg_read32(dev, PCI_CMD_REG);
    pcidev_cfg_write32(dev, PCI_CMD_REG, cmd | PCI_CMD_MEM_SPACE);
}

void pcidev_enable_io_space(pci_device_t *dev)
{
    uint32_t cmd = pcidev_cfg_read32(dev, PCI_CMD_REG);
    pcidev_cfg_write32(dev, PCI_CMD_REG, cmd | PCI_CMD_IO_SPACE);
}

void pcidev_disable_intx(pci_device_t *dev)
{
    uint32_t cmd = pcidev_cfg_read32(dev, PCI_CMD_REG);
    pcidev_cfg_write32(dev, PCI_CMD_REG, cmd | PCI_CMD_INTX_DISABLE);
}


int pcidev_find_cap(pci_device_t *dev, uint8_t cap_id)
{
    if (!dev) return 0;
    return pcie_find_cap(dev->bus, dev->dev, dev->fn, cap_id);
}

int pcidev_get_pcie_type(pci_device_t *dev)
{
    if (!dev) return -1;
    int cap = pcidev_find_cap(dev, PCI_CAP_ID_EXP);
    if (!cap) return -1;
    uint16_t v = pcidev_cfg_read16(dev, (uint16_t)(cap + 2));
    return (v >> 4) & 0x7;
}

uint16_t pcidev_read_ext_cap(pci_device_t *dev, uint16_t cap_id,
                             uint16_t start)
{
    if (!dev) return 0;
    return pcie_read_ext_cap(dev->bus, dev->dev, dev->fn, cap_id, start);
}


void pcidev_defer_device(pci_device_t *dev)
{
    if (!dev) return;
    if (dev->drv_probe_state >= 2) return;
    if (dev->drv_probe_state == 1) return;
    if (deferred_count >= PCI_DEFER_MAX) {
        dev->drv_probe_state = (pcidev_match_device(dev) == 1) ? 2 : 3;
        return;
    }
    deferred_devs[deferred_count++] = dev;
    dev->drv_probe_state = 1;
}

void pcidev_probe_all(void)
{
    if (deferred_count == 0) {
        klog(LOG_OK, "pcidev: deferred probe queue empty");
        return;
    }
    uint32_t ok = 0, fail = 0, none = 0;
    for (uint32_t i = 0; i < deferred_count; i++) {
        pci_device_t *dev = deferred_devs[i];
        if (!dev) continue;
        if (dev->drv_probe_state >= 2) continue;
        int rc = pcidev_match_device(dev);
        if (rc == 1)       { dev->drv_probe_state = 2; ok++; }
        else if (rc == 0)  { dev->drv_probe_state = 3; none++; }
        else               { dev->drv_probe_state = 3; fail++; }
    }
    deferred_count = 0;
    klog(LOG_OK, "pcidev: deferred probe done");
}


void pcidev_init(void)
{
    pcie_init();

    if (search_pci())
        klog(LOG_WARN, "pcidev: PCI scan reported error");

    pci_enumerate();
}


void pcidev_dump(void)
{
    klog(LOG_OK, "pcidev: PCI devices");
}


pci_device_t *pci_find_by_class(uint8_t cc, uint8_t sc)
{
    return pcidev_find_by_class(cc, sc);
}

pci_device_t *pci_find_by_id(uint16_t vid, uint16_t did)
{
    return pcidev_find_by_id(vid, did);
}

pci_device_t *pci_device_by_index(int index_1based)
{
    if (index_1based < 1) return NULL;
    int i = 0;
    for (pci_device_t *d = pci_device_list; d; d = d->next) {
        i++;
        if (i == index_1based) return d;
    }
    return NULL;
}

int pci_register_driver(pci_driver_t *drv)
{
    return pcidev_register_driver(drv);
}

int pci_unregister_driver(pci_driver_t *drv)
{
    return pcidev_unregister_driver(drv);
}

void pci_driver_match(pci_device_t *dev)
{
    pcidev_match_device(dev);
}

void pci_driver_defer_device(pci_device_t *dev)
{
    pcidev_defer_device(dev);
}

void pci_driver_probe_deferred_all(void)
{
    pcidev_probe_all();
}

pci_driver_t *pci_driver_find_by_name(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (pci_driver_t *d = pcidev_driver_list; d; d = d->next)
        if (streq(d->name, name)) return d;
    return NULL;
}

pci_driver_t *pci_driver_find_class_module(uint8_t class_code, uint8_t subclass,
                                           const char *module_path)
{
    if (!module_path || !module_path[0]) return NULL;
    for (pci_driver_t *d = pcidev_driver_list; d; d = d->next) {
        if (!d->module_path) continue;
        if (!streq(d->module_path, module_path)) continue;
        if (d->class_code != class_code || d->subclass != subclass) continue;
        return d;
    }
    return NULL;
}

pci_driver_t *pci_driver_find_reloc_for_device(const pci_device_t *dev)
{
    if (!dev) return NULL;
    pci_driver_t *found = NULL;
    for (pci_driver_t *d = pcidev_driver_list; d; d = d->next) {
        if (!d->probe) continue;
        if (!driver_matches(d, dev)) continue;
        if (found) return NULL;
        found = d;
    }
    return found;
}
