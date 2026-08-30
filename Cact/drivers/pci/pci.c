#include "pci.h"
#include "pcie.h"
#include "kernel.h"

// Read a 32-bit value from PCI configuration space.
// Uses PCIe ECAM when available, falls back to legacy port IO.
// reg must be DWORD-aligned (lower 2 bits are masked off).
uint32_t pci_read_config_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    if (pcie_is_available())
        return pcie_read32(bus, dev, fn, reg);

    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    port_dword_out(PCI_CONFIG_ADDRESS, addr);
    return port_dword_in(PCI_CONFIG_DATA);
}

// Write a 32-bit value to PCI configuration space.
void pci_write_config_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val) {
    if (pcie_is_available()) {
        pcie_write32(bus, dev, fn, reg, val);
        return;
    }

    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    port_dword_out(PCI_CONFIG_ADDRESS, addr);
    port_dword_out(PCI_CONFIG_DATA, val);
}

uint32_t pci_read_config_long(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return pci_read_config_dword(bus, dev, fn, reg);
}

void pci_write_config_long(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                           uint32_t val) {
    pci_write_config_dword(bus, dev, fn, reg, val);
}

#define PCI_CONFIG_COMMAND 0x04u
#define PCI_CMD_BUS_MASTER (1u << 2)

void pci_set_master(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t cmd = pci_read_config_dword(bus, dev, fn, PCI_CONFIG_COMMAND);
    pci_write_config_dword(bus, dev, fn, PCI_CONFIG_COMMAND, cmd | PCI_CMD_BUS_MASTER);
}

// Probe for PCI Mechanism #1 by writing the enable bit and reading back.
// Returns 0 if mechanism is present, 1 otherwise.
int search_pci(void) {
    port_dword_out(PCI_CONFIG_ADDRESS, 0x80000000);
    uint32_t readback = port_dword_in(PCI_CONFIG_ADDRESS);
    if (readback == 0x80000000) {
        pr_info("PCI configuration mechanism #1 (IO ports 0xCF8/0xCFC) available");
        return 0;
    }
    pr_warn("PCI config space not responding — no PCI bus?");
    return 1;
}