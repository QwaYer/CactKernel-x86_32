#include "pci_driver.h"
#include "pcie.h"
#include "pcidev.h"
#include "kernel.h"
#include "klib.h"

// Static device pool — avoids dynamic allocation during early boot
static pci_device_t  pool[MAX_PCI_DEVICES];
static uint32_t      pool_idx = 0;

// Bitmap of buses already scanned — guards the recursive bridge walk against
// circular/misconfigured secondary-bus numbers on real boards.
static uint8_t       scanned_bus_map[32];   // 256 buses
static int           scan_depth = 0;

#define MAX_SCAN_DEPTH 16

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
// A 64-bit memory BAR spans two slots: the second dword holds address bits
// 32..63, so it is read/sized/restored too, otherwise the base gets silently
// truncated to 32 bits.  The consumed upper slot is skipped via i++.
static void decode_bars(pci_device_t *d) {
    for (int i = 0; i < 6; i++) {
        uint8_t  off  = 0x10 + i * 4;
        uint32_t orig = pci_read_config_dword(d->bus, d->dev, d->fn, off);
        if (!orig) continue;

        // Is this a 64-bit memory BAR? (bits 1..2 = 10).  A 64-bit BAR needs
        // the next slot for its upper dword, so the pair must fit in slots 0..5.
        int is_64bit = i < 5 &&
                       !(orig & PCI_BAR_IO) &&
                       (((orig >> 1) & 0x3) == 0x2);

        uint32_t hi_orig = 0, hi_mask = 0;
        if (is_64bit)
            hi_orig = pci_read_config_dword(d->bus, d->dev, d->fn, off + 4);

        // BAR sizing: write ~0 (both dwords for 64-bit), read mask, restore
        pci_write_config_dword(d->bus, d->dev, d->fn, off, 0xFFFFFFFF);
        if (is_64bit)
            pci_write_config_dword(d->bus, d->dev, d->fn, off + 4, 0xFFFFFFFF);
        uint32_t mask = pci_read_config_dword(d->bus, d->dev, d->fn, off);
        if (is_64bit)
            hi_mask = pci_read_config_dword(d->bus, d->dev, d->fn, off + 4);
        pci_write_config_dword(d->bus, d->dev, d->fn, off, orig);
        if (is_64bit)
            pci_write_config_dword(d->bus, d->dev, d->fn, off + 4, hi_orig);

        if (orig & PCI_BAR_IO) {
            // I/O BAR — 4-byte aligned, lower 16 bits of size
            d->bars[i].is_io = 1;
            d->bars[i].base  = orig & 0xFFFC;
            d->bars[i].size  = (~(mask & 0xFFFC) + 1) & 0xFFFF;
        } else {
            // Memory BAR — support both 32-bit and 64-bit
            d->bars[i].is_io = 0;
            if (is_64bit) {
                uint64_t lo = orig & 0xFFFFFFF0ULL;
                uint64_t hi = (uint64_t)hi_orig << 32;
                uint64_t lo_mask = mask & 0xFFFFFFF0ULL;
                uint64_t hi_m    = (uint64_t)hi_mask << 32;
                d->bars[i].base  = hi | lo;
                d->bars[i].size  = ~(hi_m | lo_mask) + 1;
                if (d->bars[i].size == 0) d->bars[i].size = 1;
                i++; // consume the upper-32 dword slot of the 64-bit BAR
            } else {
                d->bars[i].base  = orig & 0xFFFFFFF0;
                d->bars[i].size  = ~(mask & 0xFFFFFFF0) + 1;
                if (d->bars[i].size == 0) d->bars[i].size = 1;
            }
        }
    }
}

// Forward declarations for mutual recursion (probe_fn ↔ scan_bus)
static void probe_fn(uint8_t bus, uint8_t dev, uint8_t fn);
static void scan_bus(uint8_t bus);

// Probe a single PCI function: read IDs, decode BARs, match drivers, recurse bridges
static void probe_fn(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t id = pci_read_config_dword(bus, dev, fn, 0x00);
    if (id == 0xFFFFFFFF || id == 0x00000000) return;  // absent function

    pci_device_t *d = alloc_dev();
    if (!d) { pr_warn("PCI device pool exhausted"); return; }

    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor_id = (uint16_t)(id & 0xFFFF);
    d->device_id = (uint16_t)(id >> 16);

    pr_info("PCI: %02x:%02x.%u %04x:%04x",
            (unsigned)bus, (unsigned)dev, (unsigned)fn,
            (unsigned)d->vendor_id, (unsigned)d->device_id);

    uint32_t cls  = pci_read_config_dword(bus, dev, fn, 0x08);
    d->revision   = (uint8_t) cls;
    d->prog_if    = (uint8_t)(cls >>  8);
    d->subclass   = (uint8_t)(cls >> 16);
    d->class_code = (uint8_t)(cls >> 24);

    uint32_t hdr   = pci_read_config_dword(bus, dev, fn, 0x0C);
    d->header_type = (uint8_t)(hdr >> 16);

    uint32_t irq = pci_read_config_dword(bus, dev, fn, 0x3C);
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
        uint32_t br  = pci_read_config_dword(bus, dev, fn, 0x18);
        uint8_t  sec = (uint8_t)(br >> 8);          // secondary bus number
        if (sec) scan_bus(sec);
    }

    // Defer actual probe to a scheduler-ready phase.
    pcidev_defer_device(d);
}

// Scan all functions on a given bus
static void scan_bus(uint8_t bus) {
    // Never rescan a bus and never recurse deeper than MAX_SCAN_DEPTH —
    // a corrupt secondary-bus number must not hang the boot.
    if (bus >= 256) return;
    uint8_t bit = 1u << (bus & 7);
    if (scanned_bus_map[bus >> 3] & bit) return;
    scanned_bus_map[bus >> 3] |= bit;
    if (scan_depth >= MAX_SCAN_DEPTH) return;
    scan_depth++;

    pr_info("PCI: scanning bus %u", (unsigned)bus);

    for (uint8_t dev = 0; dev < PCI_MAX_DEV; dev++) {
        // Quick check for device presence by reading vendor/device ID
        if (pci_read_config_dword(bus, dev, 0, 0x00) == 0xFFFFFFFF) continue;
        if (pci_read_config_dword(bus, dev, 0, 0x00) == 0x00000000) continue;

        probe_fn(bus, dev, 0);

        // Check header type for multi-function device
        uint32_t hdr = pci_read_config_dword(bus, dev, 0, 0x0C);
        if ((uint8_t)(hdr >> 16) & PCI_HEADER_MULTIFUNCTION) {
            for (uint8_t fn = 1; fn < PCI_MAX_FN; fn++) {
                uint32_t idn = pci_read_config_dword(bus, dev, fn, 0x00);
                if (idn == 0xFFFFFFFF || idn == 0x00000000) continue;
                probe_fn(bus, dev, fn);
            }
        }
    }

    scan_depth--;
}

// Entry point: determine host bridge type, then scan the PCI hierarchy
void pci_enumerate(void) {
    uint32_t hdr0 = pci_read_config_dword(0, 0, 0, 0x0C);
    if (!((uint8_t)(hdr0 >> 16) & PCI_HEADER_MULTIFUNCTION)) {
        scan_bus(0);
    } else {
        for (uint8_t fn = 0; fn < PCI_MAX_FN; fn++) {
            uint32_t id = pci_read_config_dword(0, 0, fn, 0x00);
            if (id == 0xFFFFFFFF || id == 0x00000000) break;
            scan_bus(fn);
        }
    }
    pr_info("PCI bus enumeration finished");
}

void pci_enum_dump(void) {
    for (pci_device_t *d = pci_device_list; d; d = d->next) { (void)d; }
}