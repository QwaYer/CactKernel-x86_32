#include "pci_driver.h"
#include "pci_loader.h"
#include "kernel.h"
#include "libc.h"

static pci_driver_t *driver_list  = NULL;
static uint32_t      driver_count = 0;

static int driver_matches(const pci_driver_t *drv, const pci_device_t *dev) {
    if (drv->vendor_id  != PCI_ANY_ID          && drv->vendor_id  != dev->vendor_id)  return 0;
    if (drv->device_id  != PCI_ANY_ID          && drv->device_id  != dev->device_id)  return 0;
    if (drv->class_code != (uint8_t)PCI_ANY_ID && drv->class_code != dev->class_code) return 0;
    if (drv->subclass   != (uint8_t)PCI_ANY_ID && drv->subclass   != dev->subclass)   return 0;
    return 1;
}


//Public api
int pci_register_driver(pci_driver_t *drv) {
    if (!drv || !drv->probe) return -1;
    if (driver_count >= MAX_PCI_DRIVERS) {
        kprint("[DRV] Driver table full\n");
        return -1;
    }
    for (pci_driver_t *d = driver_list; d; d = d->next) {
        if (strcmp(d->name, drv->name) == 0) {
            kprint("[DRV] Duplicate driver: "); kprint(drv->name); kprint("\n");
            return -1;
        }
    }
    drv->next   = driver_list;
    driver_list = drv;
    driver_count++;
    return 0;
}

int pci_unregister_driver(pci_driver_t *drv) {
    pci_driver_t **pp = &driver_list;
    while (*pp) {
        if (*pp == drv) {
            *pp = drv->next;
            driver_count--;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

void pci_driver_match(pci_device_t *dev) {
    for (pci_driver_t *drv = driver_list; drv; drv = drv->next) {
        if (!driver_matches(drv, dev)) continue;

        if (drv->module_path && !drv->probe) {
            if (pci_load_module(drv->module_path, drv) != 0) {
                kprint("[DRV] Module load failed: ");
                kprint((char *)drv->module_path); kprint("\n");
                continue;
            }
        }

        if (drv->probe(dev) != 0)
            kprint("[DRV] probe() failed\n");

        return;
    }
}

void pci_driver_dump(void) {
    kprint("[DRV] Registered drivers:\n");
    for (pci_driver_t *d = driver_list; d; d = d->next) {
        kprint("  "); kprint(d->name);
        kprint(" VID="); kprint_hex(d->vendor_id);
        kprint(" DID="); kprint_hex(d->device_id);
        kprint(" CC=");  kprint_hex(d->class_code);
        kprint("/");     kprint_hex(d->subclass);
        if (d->module_path) { kprint(" mod="); kprint((char *)d->module_path); }
        kprint("\n");
    }
}