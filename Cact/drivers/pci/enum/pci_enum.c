#include "pci_driver.h"
#include "pci_gdd.h"
#include "pcie.h"
#include "pcidev.h"
#include "kernel.h"
#include "klib.h"

// Static device pool — avoids dynamic allocation during early boot
static pci_device_t  pool[MAX_PCI_DEVICES];
static uint32_t      pool_idx = 0;

// Allocate a device descriptor from the static pool
static pci_device_t *alloc_dev(void) {
    if (pool_idx >= MAX_PCI_DEVICES) return NULL;
    pci_device_t *d = &pool[pool_idx++];
    memset(d, 0, sizeof *d);
    return d;
}

// Append device to the tail of the global list
static void list_push(pci_device_t *d) {
    d->next = NULL;
    if (!pci_device_list) {
        pci_device_list = d;
    } else {
        pci_device_t *c = pci_device_list;
        while (c->next) c = c->next;
        c->next = d;
    }
    pci_device_count++;
}

// Probe a device's BARs: write all-ones, read back mask, determine base/size/type.
// Skips 64-bit BARs (type != PCI_BAR_MEM_TYPE_32) by consuming the next slot.
static void decode_bars(pci_device_t *d) {
    for (int i = 0; i < 6; i++) {
        uint8_t  off  = 0x10 + i * 4;
        uint32_t orig = pci_read32(d->bus, d->dev, d->fn, off);
        if (!orig) continue;

        // BAR sizing: write ~0, read mask, restore original
        pci_write32(d->bus, d->dev, d->fn, off, 0xFFFFFFFF);
        uint32_t mask = pci_read32(d->bus, d->dev, d->fn, off);
        pci_write32(d->bus, d->dev, d->fn, off, orig);

        if (orig & PCI_BAR_IO) {
            // I/O BAR — 4-byte aligned, lower 16 bits of size
            d->bars[i].is_io = 1;
            d->bars[i].base  = orig & 0xFFFC;
            d->bars[i].size  = (~(mask & 0xFFFC) + 1) & 0xFFFF;
        } else {
            // Memory BAR — 32-bit only; skip 64-bit BARs
            uint8_t type = (orig >> 1) & 0x3;
            if (type != PCI_BAR_MEM_TYPE_32) { i++; continue; }
            d->bars[i].is_io = 0;
            d->bars[i].base  = orig & 0xFFFFFFF0;
            d->bars[i].size  = ~(mask & 0xFFFFFFF0) + 1;
        }
    }
}

// Forward declarations for mutual recursion (probe_fn ↔ scan_bus)
static void probe_fn(uint8_t bus, uint8_t dev, uint8_t fn);
static void scan_bus(uint8_t bus);

// Probe a single PCI function: read IDs, decode BARs, match drivers, recurse bridges
static void probe_fn(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t id = pci_read32(bus, dev, fn, 0x00);
    if (id == 0xFFFFFFFF || id == 0x00000000) return;  // absent function

    pci_device_t *d = alloc_dev();
    if (!d) { klog(LOG_WARN, "PCI device pool exhausted"); return; }

    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor_id = (uint16_t)(id & 0xFFFF);
    d->device_id = (uint16_t)(id >> 16);

    uint32_t cls  = pci_read32(bus, dev, fn, 0x08);
    d->revision   = (uint8_t) cls;
    d->prog_if    = (uint8_t)(cls >>  8);
    d->subclass   = (uint8_t)(cls >> 16);
    d->class_code = (uint8_t)(cls >> 24);

    uint32_t hdr   = pci_read32(bus, dev, fn, 0x0C);
    d->header_type = (uint8_t)(hdr >> 16);

    uint32_t irq = pci_read32(bus, dev, fn, 0x3C);
    d->irq_line  = (uint8_t) irq;
    d->irq_pin   = (uint8_t)(irq >> 8);

    // Detect PCIe capability
    d->pcie_type = -1;
    if (pcie_is_available()) {
        int cap_off = pcie_find_cap(bus, dev, fn, PCI_CAP_ID_EXP);
        if (cap_off) {
            uint16_t cap_reg = pcie_read16(bus, dev, fn, cap_off + 2);
            d->pcie_type = (int8_t)((cap_reg >> 4) & 0x7);
        }
    }

    // Only decode BARs for normal (non-bridge) headers
    if ((d->header_type & 0x7F) == PCI_HEADER_TYPE_NORMAL)
        decode_bars(d);

    list_push(d);

    // If this is a PCI-to-PCI bridge, recursively scan the secondary bus
    if (d->class_code == PCI_CLASS_BRIDGE     &&
        d->subclass   == PCI_SUBCLASS_PCI_BRIDGE &&
        (d->header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE)
    {
        uint32_t br  = pci_read32(bus, dev, fn, 0x18);
        uint8_t  sec = (uint8_t)(br >> 8);          // secondary bus number
        if (sec) scan_bus(sec);
    }

    // Allow GDD to register lazy module mappings while still in early stage.
    pci_user_prompt_module(d->class_code, d->subclass, d->prog_if, d);
    // Defer actual probe to a scheduler-ready phase.
    pcidev_defer_device(d);
}

// Scan all functions on a given bus
static void scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < PCI_MAX_DEV; dev++) {
        // Quick check for device presence by reading vendor/device ID
        if (pci_read32(bus, dev, 0, 0x00) == 0xFFFFFFFF) continue;
        if (pci_read32(bus, dev, 0, 0x00) == 0x00000000) continue;

        probe_fn(bus, dev, 0);

        // Check header type for multi-function device
        uint32_t hdr = pci_read32(bus, dev, 0, 0x0C);
        if ((uint8_t)(hdr >> 16) & PCI_HEADER_MULTIFUNCTION) {
            for (uint8_t fn = 1; fn < PCI_MAX_FN; fn++) {
                uint32_t idn = pci_read32(bus, dev, fn, 0x00);
                if (idn == 0xFFFFFFFF || idn == 0x00000000) continue;
                probe_fn(bus, dev, fn);
            }
        }
    }
}

// Entry point: determine host bridge type, then scan the PCI hierarchy
void pci_enumerate(void) {
    uint32_t hdr0 = pci_read32(0, 0, 0, 0x0C);
    if (!((uint8_t)(hdr0 >> 16) & PCI_HEADER_MULTIFUNCTION)) {
        scan_bus(0);
    } else {
        for (uint8_t fn = 0; fn < PCI_MAX_FN; fn++) {
            uint32_t id = pci_read32(0, 0, fn, 0x00);
            if (id == 0xFFFFFFFF || id == 0x00000000) break;
            scan_bus(fn);
        }
    }
    klog(LOG_OK, "PCI bus enumeration finished");
}

void pci_enum_dump(void) {
    for (pci_device_t *d = pci_device_list; d; d = d->next) { (void)d; }
}