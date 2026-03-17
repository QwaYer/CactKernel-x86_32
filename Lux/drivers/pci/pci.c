#include "pci.h"
#include "kernel.h"

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    port_dword_out(PCI_CONFIG_ADDRESS, addr);
    return port_dword_in(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    port_dword_out(PCI_CONFIG_ADDRESS, addr);
    port_dword_out(PCI_CONFIG_DATA, val);
}

int pci_find_nvme(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn) {
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32(bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF || id == 0x00000000) continue;

                uint32_t cls = pci_read32(bus, dev, fn, 0x08);
                uint8_t  cc  = (uint8_t)((cls >> 24) & 0xFF);
                uint8_t  sc  = (uint8_t)((cls >> 16) & 0xFF);
                uint8_t  pi  = (uint8_t)((cls >>  8) & 0xFF);

                // class 0x01 (Mass Storage), subclass 0x08 (NVM), prog-if 0x02 (NVMe)
                if (cc == 0x01 && sc == 0x08 && pi == 0x02) {
                    uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
                    pci_write32(bus, dev, fn, 0x04, cmd | 0x06);
                    *out_bus = bus;
                    *out_dev = dev;
                    *out_fn  = fn;
                    return 1;
                }
            }
        }
    }

    kprint("[PCI] NVMe controller NOT found\n");
    return 0;
}

int search_pci(void) {
    port_dword_out(PCI_CONFIG_ADDRESS, 0x80000000);
    return (port_dword_in(PCI_CONFIG_ADDRESS) == 0x80000000) ? 0 : 1;
}
