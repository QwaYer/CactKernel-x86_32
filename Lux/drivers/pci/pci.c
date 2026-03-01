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

uint16_t pci_find_ide_bm_base(void) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_read32(0, dev, fn, 0x00);
            if (id == 0xFFFFFFFF || id == 0x00000000) continue;

            uint32_t cls = pci_read32(0, dev, fn, 0x08);
            uint8_t  cc  = (uint8_t)((cls >> 24) & 0xFF);
            uint8_t  sc  = (uint8_t)((cls >> 16) & 0xFF);
            if (cc != 0x01 || sc != 0x01) continue;

            uint32_t cmd = pci_read32(0, dev, fn, 0x04);
            if (!(cmd & 0x05))
                pci_write32(0, dev, fn, 0x04, cmd | 0x05);

            uint32_t bar4 = pci_read32(0, dev, fn, 0x20);
            uint16_t bm_base = (uint16_t)(bar4 & 0xFFFC);

            if (!bm_base) {
                pci_write32(0, dev, fn, 0x20, 0xC001);
                bar4    = pci_read32(0, dev, fn, 0x20);
                bm_base = (uint16_t)(bar4 & 0xFFFC);
            }

            if (!bm_base) continue;

            kprint("[PCI] IDE Bus Master found, BM base = 0x");
            kprint_hex(bm_base);
            kprint("\n");
            return bm_base;
        }
    }

    kprint("[PCI] IDE Bus Master NOT found\n");
    return 0;
}


void pci_dump_all(void) {
    kprint("[PCI] Full config dump:\n");

    char buf[12];
    for (uint8_t fn = 0; fn < 8; fn++) {
        uint32_t id  = pci_read32(0, 1, fn, 0x00);
        uint32_t cls = pci_read32(0, 1, fn, 0x08);
        uint32_t cmd = pci_read32(0, 1, fn, 0x04);
        uint32_t bar0 = pci_read32(0, 1, fn, 0x10);
        uint32_t bar4 = pci_read32(0, 1, fn, 0x20);
        kprint("  dev=1 fn="); itoa(fn, buf); kprint(buf);
        kprint(" id=0x"); kprint_hex(id);
        kprint(" cls=0x"); kprint_hex(cls);
        kprint(" cmd=0x"); kprint_hex(cmd);
        kprint(" BAR0=0x"); kprint_hex(bar0);
        kprint(" BAR4=0x"); kprint_hex(bar4);
        kprint("\n");
    }

    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_read32(0, dev, fn, 0x00);
            if (id == 0xFFFFFFFF || id == 0x00000000) continue;
            uint32_t cls  = pci_read32(0, dev, fn, 0x08);
            uint32_t bar4 = pci_read32(0, dev, fn, 0x20);
            uint8_t  cc   = (uint8_t)((cls >> 24) & 0xFF);
            uint8_t  sc   = (uint8_t)((cls >> 16) & 0xFF);
            kprint("  dev="); itoa(dev, buf); kprint(buf);
            kprint(" fn="); itoa(fn, buf); kprint(buf);
            kprint(" id=0x"); kprint_hex(id);
            kprint(" cc=0x"); kprint_hex(cc);
            kprint(" sc=0x"); kprint_hex(sc);
            kprint(" BAR4=0x"); kprint_hex(bar4);
            kprint("\n");
        }
    }
}
int search_pci(void) {
    port_dword_out(PCI_CONFIG_ADDRESS, 0x80000000);
    return (port_dword_in(PCI_CONFIG_ADDRESS) == 0x80000000) ? 0 : 1;
}