#include "pcie.h"
#include "pci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "pci_enum.h"
#include "acpi.h"
#include "cact_acpi.h"

typedef struct {
    uint32_t           base_addr;
    uint16_t           segment;
    uint8_t            start_bus;
    uint8_t            end_bus;
    volatile uint8_t  *virt;
} pcie_ecam_t;

static pcie_ecam_t pcie_ecams[PCIE_MAX_SEGMENTS];
static int         pcie_ecam_count = 0;
static int         pcie_ready = 0;

static pcie_ecam_t *pcie_find_ecam(uint8_t bus)
{
    for (int i = 0; i < pcie_ecam_count; i++)
        if (bus >= pcie_ecams[i].start_bus && bus <= pcie_ecams[i].end_bus)
            return &pcie_ecams[i];
    return NULL;
}

static inline uint32_t pcie_ecam_offset(uint8_t bus, uint8_t dev, uint8_t fn,
                                        uint16_t reg, uint8_t start_bus)
{
    return ((uint32_t)(bus - start_bus) << 20)
         | ((uint32_t)dev << 15)
         | ((uint32_t)fn  << 12)
         | (reg & 0xFFF);
}

uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    pcie_ecam_t *ecam = pcie_find_ecam(bus);
    if (!ecam) return 0xFFFFFFFF;
    uint32_t off = pcie_ecam_offset(bus, dev, fn, reg, ecam->start_bus);
    return *(volatile uint32_t *)(ecam->virt + off);
}

void pcie_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint32_t val)
{
    pcie_ecam_t *ecam = pcie_find_ecam(bus);
    if (!ecam) return;
    uint32_t off = pcie_ecam_offset(bus, dev, fn, reg, ecam->start_bus);
    *(volatile uint32_t *)(ecam->virt + off) = val;
}

uint8_t pcie_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    pcie_ecam_t *ecam = pcie_find_ecam(bus);
    if (!ecam) return 0xFF;
    uint32_t off = pcie_ecam_offset(bus, dev, fn, reg, ecam->start_bus);
    return *(volatile uint8_t *)(ecam->virt + off);
}

uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    pcie_ecam_t *ecam = pcie_find_ecam(bus);
    if (!ecam) return 0xFFFF;
    uint32_t off = pcie_ecam_offset(bus, dev, fn, reg, ecam->start_bus);
    return *(volatile uint16_t *)(ecam->virt + off);
}

void pcie_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint8_t val)
{
    pcie_ecam_t *ecam = pcie_find_ecam(bus);
    if (!ecam) return;
    uint32_t off = pcie_ecam_offset(bus, dev, fn, reg, ecam->start_bus);
    *(volatile uint8_t *)(ecam->virt + off) = val;
}

void pcie_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint16_t val)
{
    pcie_ecam_t *ecam = pcie_find_ecam(bus);
    if (!ecam) return;
    uint32_t off = pcie_ecam_offset(bus, dev, fn, reg, ecam->start_bus);
    *(volatile uint16_t *)(ecam->virt + off) = val;
}

int pcie_find_cap(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t cap_id)
{
    uint8_t cap_ptr = (uint8_t)(pci_read_config_dword(bus, dev, fn, 0x34) & 0xFF);
    while (cap_ptr && cap_ptr != 0xFF) {
        uint32_t cap = pcie_read32(bus, dev, fn, cap_ptr);
        if ((uint8_t)(cap & 0xFF) == cap_id)
            return (int)cap_ptr;
        cap_ptr = (uint8_t)((cap >> 8) & 0xFF);
    }
    return 0;
}

int pcie_get_type(uint8_t bus, uint8_t dev, uint8_t fn)
{
    int cap_off = pcie_find_cap(bus, dev, fn, PCIE_CAP_ID);
    if (!cap_off) return -1;
    uint16_t cap_reg = pcie_read16(bus, dev, fn, cap_off + 2);
    return (cap_reg >> 4) & 0x7;
}

uint16_t pcie_read_ext_cap(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint16_t cap_id, uint16_t offset)
{
    if (offset == 0) {
        uint16_t first = pcie_read16(bus, dev, fn, 0x100);
        uint16_t next = (first >> 4) & 0xFFF;
        uint16_t id  = first & 0xFFF;
        if (id == cap_id) return pcie_read16(bus, dev, fn, 0x102);
        offset = next;
    }
    while (offset > 0 && offset != 0xFFF) {
        uint16_t cap = pcie_read16(bus, dev, fn, offset);
        uint16_t id  = cap & 0xFFF;
        if (id == cap_id)
            return pcie_read16(bus, dev, fn, offset + 2);
        offset = (cap >> 4) & 0xFFF;
    }
    return 0;
}

void pcie_dump_all(void)
{
    if (!pcie_ready) {
        pr_warn("PCIe: not available");
        return;
    }
    for (pci_device_t *d = pci_device_list; d; d = d->next) {
        int cap = pcie_find_cap(d->bus, d->dev, d->fn, PCIE_CAP_ID);
        if (!cap) continue;
        pr_info("PCIe device detected");
    }
}

bool pcie_init(void)
{
    ACPI_TABLE_MCFG *mcfg = NULL;
    ACPI_STATUS status = AcpiGetTable("MCFG", 1, (ACPI_TABLE_HEADER **)&mcfg);

    if (ACPI_FAILURE(status) || !mcfg) {
        pr_warn("PCIe: MCFG table not found — using legacy PCI");
        return false;
    }

    ACPI_MCFG_ALLOCATION *entry = (ACPI_MCFG_ALLOCATION *)(mcfg + 1);
    uint32_t table_len = mcfg->Header.Length;
    uint32_t offset    = sizeof(ACPI_TABLE_MCFG);

    int mapped = 0;
    while (offset + sizeof(ACPI_MCFG_ALLOCATION) <= table_len &&
           pcie_ecam_count < PCIE_MAX_SEGMENTS)
    {
        uint32_t base      = (uint32_t)entry->Address;
        uint8_t  start_bus = entry->StartBusNumber;
        uint8_t  end_bus   = entry->EndBusNumber;

        uint32_t bus_count   = end_bus - start_bus + 1;
        uint32_t region_size;
        if (bus_count > 4095u) {
            pr_warn("PCIe ECAM region_size overflow, skipping segment\n");
            entry++;
            offset += sizeof(ACPI_MCFG_ALLOCATION);
            continue;
        }
        region_size = bus_count * 32 * 8 * 4096;
        uint32_t pages       = (region_size + 0xFFF) >> 12;

        uint32_t seg_shift = pcie_ecam_count * 0x10000000;
        uint32_t vaddr     = PCIE_ECAM_VADDR + seg_shift;

        for (uint32_t i = 0; i < pages; i++)
            vmm_map(get_current_pd(), vaddr + i * 4096, base + i * 4096,
                    PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

        pcie_ecams[pcie_ecam_count].base_addr = base;
        pcie_ecams[pcie_ecam_count].segment   = entry->PciSegment;
        pcie_ecams[pcie_ecam_count].start_bus = start_bus;
        pcie_ecams[pcie_ecam_count].end_bus   = end_bus;
        pcie_ecams[pcie_ecam_count].virt      = (volatile uint8_t *)vaddr;
        pcie_ecam_count++;
        mapped++;

        entry++;
        offset += sizeof(ACPI_MCFG_ALLOCATION);
    }

    if (mapped > 0) {
        pcie_ready = 1;
        pr_info("PCIe: ECAM enabled");
        return true;
    }

    pr_warn("PCIe: MCFG table invalid");
    return false;
}

bool pcie_is_available(void)
{
    return pcie_ready != 0;
}
