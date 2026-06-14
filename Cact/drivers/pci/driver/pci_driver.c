#include "pci_driver.h"
#include "pci_loader.h"
#include "pci_enum.h"
#include "kernel.h"
#include "klib.h"

// Singly-linked list of registered drivers
static pci_driver_t *driver_list  = NULL;
static uint32_t      driver_count = 0;
#define PCI_DEFER_MAX_DEVICES  MAX_PCI_DEVICES
static pci_device_t *deferred_devs[PCI_DEFER_MAX_DEVICES];
static uint32_t      deferred_count = 0;

// Check whether a driver claims a given PCI device.
// PCI_ANY_ID (0xFFFF) acts as a wildcard for vendor/device/class/subclass.
static int driver_matches(const pci_driver_t *drv, const pci_device_t *dev) {
    if (drv->vendor_id  != PCI_ANY_ID          && drv->vendor_id  != dev->vendor_id)  return 0;
    if (drv->device_id  != PCI_ANY_ID          && drv->device_id  != dev->device_id)  return 0;
    if (drv->class_code != (uint8_t)PCI_ANY_ID && drv->class_code != dev->class_code) return 0;
    if (drv->subclass   != (uint8_t)PCI_ANY_ID && drv->subclass   != dev->subclass)   return 0;
    return 1;
}

// Register a PCI driver. Duplicate names are rejected.
// `probe` may be NULL if `module_path` is set — pci_driver_match() will lazy-load.
int pci_register_driver(pci_driver_t *drv) {
    if (!drv || (!drv->probe && !drv->module_path)) return -1;
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

// Unregister a PCI driver (must match exact pointer).
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

pci_driver_t *pci_driver_find_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    for (pci_driver_t *d = driver_list; d; d = d->next)
        if (streq(d->name, name)) return d;
    return NULL;
}

pci_driver_t *pci_driver_find_class_module(uint8_t class_code, uint8_t subclass,
                                           const char *module_path) {
    if (!module_path || !module_path[0]) return NULL;
    for (pci_driver_t *d = driver_list; d; d = d->next) {
        if (!d->module_path) continue;
        if (!streq(d->module_path, module_path)) continue;
        if (d->class_code != class_code || d->subclass != subclass) continue;
        return d;
    }
    return NULL;
}

pci_driver_t *pci_driver_find_reloc_for_device(const pci_device_t *dev) {
    if (!dev) return NULL;
    pci_driver_t *found = NULL;
    for (pci_driver_t *d = driver_list; d; d = d->next) {
        if (!(d->flags & PCI_DRV_F_RELOC_MODULE)) continue;
        if (!d->probe) continue;
        if (!driver_matches(d, dev)) continue;
        if (found) return NULL;
        found = d;
    }
    return found;
}

// Walk the driver list and invoke the first matching probe.
// If the driver has a module_path but no probe yet, attempt lazy module load.
static int pci_driver_match_internal(pci_device_t *dev) {
    for (pci_driver_t *drv = driver_list; drv; drv = drv->next) {
        if (!driver_matches(drv, dev)) continue;

        // Lazy-load module if path is set and probe is not yet populated
        if (drv->module_path && !drv->probe) {
            if (pci_load_module(drv->module_path, drv) != 0) {
                kprint("[DRV] Module load failed: ");
                kprint((char *)drv->module_path); kprint("\n");
                continue;
            }
        }

        if (!drv->probe) {
            kprint("[DRV] driver "); kprint(drv->name);
            kprint(" matched PCI but probe is NULL\n");
            return -1;
        }

        kprint("[DRV] "); kprint(drv->name);
        kprint(" attached "); kprint_hex(dev->vendor_id);
        kprint(":"); kprint_hex(dev->device_id);
        kprint(" @"); kprint_hex(dev->bus); kprint(":");
        kprint_hex(dev->dev); kprint("."); kprint_hex(dev->fn);
        kprint("\n");

        if (drv->probe(dev) != 0) {
            kprint("[DRV] probe() returned error\n");
            return -1;
        }
        return 1;
    }
    return 0;
}

void pci_driver_match(pci_device_t *dev) {
    (void)pci_driver_match_internal(dev);
}

void pci_driver_defer_device(pci_device_t *dev) {
    if (!dev) return;
    if (dev->drv_probe_state >= 2) return;
    if (dev->drv_probe_state == 1) return;
    if (deferred_count >= PCI_DEFER_MAX_DEVICES) {
        kprint("[DRV] deferred queue full, probing inline\n");
        int rc = pci_driver_match_internal(dev);
        dev->drv_probe_state = (rc == 1) ? 2 : 3;
        return;
    }
    deferred_devs[deferred_count++] = dev;
    dev->drv_probe_state = 1;
}

void pci_driver_probe_deferred_all(void) {
    if (deferred_count == 0) {
        klog(LOG_OK, "PCI deferred driver probe: queue empty");
        return;
    }
    uint32_t failed = 0;
    uint32_t no_driver = 0;
    for (uint32_t i = 0; i < deferred_count; i++) {
        pci_device_t *dev = deferred_devs[i];
        if (!dev) continue;
        int rc = pci_driver_match_internal(dev);
        if (rc == 1) {
            dev->drv_probe_state = 2;
        } else if (rc == 0) {
            dev->drv_probe_state = 3;
            no_driver++;
        } else {
            dev->drv_probe_state = 3;
            failed++;
        }
    }
    deferred_count = 0;
    char a[16], b[16];
    itoa((int)failed, a);
    itoa((int)no_driver, b);
    kprint("[DRV] deferred done: failed=");
    kprint(a);
    kprint(" no-driver=");
    kprint(b);
    kprint("\n");
    klog(LOG_OK, "PCI deferred driver probe finished");
}

// Dump all registered drivers to debug output.
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

// Drivers (~480 each) + devices (~380 each), up to MAX_PCI_DRIVERS + MAX_PCI_DEVICES
#define PCI_MODINFO_MAX 32768

int pci_driver_modinfo_read(uint32_t off, uint32_t size, char *buf) {
    static char tmp[PCI_MODINFO_MAX];

    uint32_t nreg = 0;
    for (pci_driver_t *x = driver_list; x; x = x->next) nreg++;

    int p = 0;
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# /dev/modinfo - PCI overview\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# A: structs from pci_register_driver()  ");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "B: every function found by pci_enumerate()\n\n");

    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# --- A. pci_register_driver() ---\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# Drivers registered: ");
    p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)nreg);
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "  (0xFFFF = PCI_ANY_ID)\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# \"probe bound\" = entry point ready ");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "(built-in or module loaded)\n\n");

    int idx = 0;
    char hx[16];
    for (pci_driver_t *d = driver_list; d; d = d->next) {
        idx++;
        if (p >= PCI_MODINFO_MAX - 520) {
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n# ... A truncated ...\n");
            break;
        }

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "[drv ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, idx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "] ");
        p = buf_append(tmp, p, PCI_MODINFO_MAX, d->name);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    vendor_id:       ");
        hex_to_ascii((unsigned int)d->vendor_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    device_id:       ");
        hex_to_ascii((unsigned int)d->device_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    class_code:      ");
        hex_to_ascii((unsigned int)d->class_code, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    subclass:        ");
        hex_to_ascii((unsigned int)d->subclass, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    module_path:     ");
        if (d->module_path)
            p = buf_append(tmp, p, PCI_MODINFO_MAX, (char *)d->module_path);
        else
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "(none - built-in or no lazy path)");
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    probe bound:     ");
        if (d->probe)
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "yes\n");
        else
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "no (lazy load not done yet)\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    kmod unload:     ");
        if (d->flags & PCI_DRV_F_RELOC_MODULE)
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "yes (sys_module_unload \"");
        else
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "no (built-in, not ET_REL)\n");
        if (d->flags & PCI_DRV_F_RELOC_MODULE) {
            p = buf_append(tmp, p, PCI_MODINFO_MAX, d->name);
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "\")\n");
        }

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");
    }

    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# --- B. pci_enumerate() - PCI functions ---\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# Devices in list: ");
    p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)pci_device_count);
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n\n");

    int bidx = 0;
    for (pci_device_t *dv = pci_device_list; dv; dv = dv->next) {
        bidx++;
        if (p >= PCI_MODINFO_MAX - 400) {
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n# ... B truncated ...\n");
            break;
        }

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "[pci ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, bidx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "] ");
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "bus ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->bus);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, " dev ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->dev);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, " fn ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->fn);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    vendor_id:       ");
        hex_to_ascii((unsigned int)dv->vendor_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    device_id:       ");
        hex_to_ascii((unsigned int)dv->device_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    class_code:      ");
        hex_to_ascii((unsigned int)dv->class_code, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    subclass:        ");
        hex_to_ascii((unsigned int)dv->subclass, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    prog_if:         ");
        hex_to_ascii((unsigned int)dv->prog_if, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    revision:        ");
        hex_to_ascii((unsigned int)dv->revision, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    header_type:     ");
        hex_to_ascii((unsigned int)dv->header_type, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    irq_line:        ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->irq_line);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    irq_pin:         ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->irq_pin);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n\n");
    }

    int total = 0;
    while (tmp[total] && total < PCI_MODINFO_MAX - 1) total++;

    if (off >= (uint32_t)total) return 0;
    uint32_t avail = (uint32_t)total - off;
    if (size > avail) size = avail;
    memcpy(buf, tmp + off, size);
    return (int)size;
}