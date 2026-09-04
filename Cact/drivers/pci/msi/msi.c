#include "msi.h"
#include "pci.h"
#include "pcidev.h"
#include "pcie.h"
#include "kernel.h"
#include "memory.h"
#include "apic.h"
#include "idt.h"
#include "klib.h"

#define MSIX_VECTOR_NONE    0xFF

static void (*msix_handlers[MSIX_VECTOR_COUNT])(void);
static unsigned char msix_vector_alloc[MSIX_VECTOR_COUNT];
static int msix_initialized = 0;

extern uint32_t msix_stub_table[];

/* MMIO (BAR/MSI-X) mappings must live in the global kernel page directory,
 * never in whatever process PD happens to be active when the deferred driver
 * probe runs — otherwise the IRQ handler faults under a user CR3. */
extern uint32_t page_directory[1024];

void msix_init(void)
{
    memset(msix_handlers, 0, sizeof(msix_handlers));
    memset(msix_vector_alloc, 0, sizeof(msix_vector_alloc));

    /* Vector 0x80 is not allocatable: it is the int-0x80 syscall gate and
     * must never be overwritten by an MSI-X stub. */
    unsigned int syscall_idx = 0x80 - MSIX_VECTOR_BASE;
    if (syscall_idx < MSIX_VECTOR_COUNT)
        msix_vector_alloc[syscall_idx] = 1;

    msix_initialized = 1;
    pr_info("MSI-X: vector pool 0x30-0xEF ready");
}

int msix_alloc_vector(void)
{
    if (!msix_initialized) return -1;
    for (unsigned int i = 0; i < MSIX_VECTOR_COUNT; i++) {
        if (!msix_vector_alloc[i]) {
            msix_vector_alloc[i] = 1;
            return MSIX_VECTOR_BASE + i;
        }
    }
    pr_warn("MSI-X: no free vectors");
    return -1;
}

void msix_free_vector(int vector)
{
    if (vector < MSIX_VECTOR_BASE || vector >= MSIX_VECTOR_END) return;
    unsigned int idx = vector - MSIX_VECTOR_BASE;
    msix_vector_alloc[idx] = 0;
    msix_handlers[idx] = NULL;
}

int msix_register_handler(int vector, void (*handler)(void))
{
    if (!msix_initialized) return -1;
    if (vector < MSIX_VECTOR_BASE || vector >= MSIX_VECTOR_END) return -1;
    if (!handler) return -1;

    unsigned int idx = vector - MSIX_VECTOR_BASE;
    if (msix_handlers[idx]) return -1;
    msix_handlers[idx] = handler;
    set_idt_gate(vector, msix_stub_table[idx]);
    return 0;
}

void msix_unregister_handler(int vector)
{
    if (vector < MSIX_VECTOR_BASE || vector >= MSIX_VECTOR_END) return;
    unsigned int idx = vector - MSIX_VECTOR_BASE;
    msix_handlers[idx] = NULL;
}

void msix_dispatch(unsigned int vector)
{
    if (vector < MSIX_VECTOR_BASE || vector >= MSIX_VECTOR_END) return;
    unsigned int idx = vector - MSIX_VECTOR_BASE;
    void (*h)(void) = msix_handlers[idx];
    if (h) h();
}

int pci_msix_support(pci_device_t *dev)
{
    if (!dev) return 0;

    /* Capabilities pointer: byte 0x34 of the type-0 header.  Read it as a
     * byte (alignment-safe on both ECAM and port-IO). */
    uint8_t cap_ptr = pcidev_cfg_read8(dev, 0x34);
    int iter = 0;

    /* Walk the capabilities linked list.  Per the PCI spec the pointer is
     * always >= 0x40; anything else means the list is absent. */
    while (cap_ptr >= 0x40 && cap_ptr != 0xFF && iter++ < 48) {
        uint8_t cap_id = pcidev_cfg_read8(dev, cap_ptr);
        if (cap_id == PCI_CAP_ID_MSIX) {
            pr_info("MSI-X: capability at 0x%x (pcie=%d)", (int)cap_ptr, (int)pcie_is_available());
            return (int)cap_ptr;
        }
        cap_ptr = pcidev_cfg_read8(dev, (uint16_t)(cap_ptr + 1));   /* NextPtr */
    }

    /*
     * Fallback: some BIOSes/devices expose a broken or absent capability
     * list pointer while the MSI-X capability still sits in the capability
     * region.  Scan 0x40..0xFF for the MSI-X signature and sanity-check the
     * message-control table size so a stray 0x11 byte cannot false-positive.
     */
    for (unsigned int off = 0x40; off < 0x100; off += 4) {
        uint8_t cap_id = pcidev_cfg_read8(dev, (uint16_t)off);
        if (cap_id != PCI_CAP_ID_MSIX)
            continue;
        uint8_t next = pcidev_cfg_read8(dev, (uint16_t)(off + 1));
        uint16_t msg_ctrl = pcidev_cfg_read16(dev, (uint16_t)(off + 2));
        if ((next == 0x00 || next >= 0x40) && (msg_ctrl & 0x7FF) < 2048) {
            pr_info("MSI-X: capability found by scan at 0x%x (pcie=%d)", (int)off, (int)pcie_is_available());
            return (int)off;
        }
    }

    pr_warn("MSI-X: capability NOT found (pcie=%d, cap_ptr=0x%x, vid=0x%x did=0x%x)",
            (int)pcie_is_available(), (int)cap_ptr,
            (unsigned)dev->vendor_id, (unsigned)dev->device_id);
    return 0;
}

int pci_msix_table_map(pci_device_t *dev,
                       volatile struct msix_table_entry **table_out,
                       uint32_t *table_size_out)
{
    int cap_off = pci_msix_support(dev);
    if (!cap_off) return -1;

    /* MSG_CTRL is a 16-bit field at cap_off+2.  Reading it as a dword is
     * wrong: ECAM config access does not force 4-byte alignment, so a
     * dword read at cap_off+2 would grab MSG_CTRL|TBL_OFFSET and the table
     * size would come from garbage.  Use the 16-bit accessor. */
    uint16_t msg_ctrl = pcidev_cfg_read16(dev, (uint16_t)(cap_off + 2));
    uint32_t t_off    = pci_read_config_dword(dev->bus, dev->dev, dev->fn, cap_off + 4);

    unsigned int table_bir = t_off & 0x7;
    uint32_t table_offset  = t_off & ~0x7u;
    unsigned int table_size = (msg_ctrl & 0x7FF) + 1;

    if (table_bir >= 6) return -1;

    pci_bar_t *bar = &dev->bars[table_bir];
    if (bar->is_io || !bar->base) return -1;

    /* Do the whole address computation in 64 bits — the BAR is 64-bit typed
     * and table_offset is only a 32-bit offset into it. */
    uint64_t table_addr = bar->base + table_offset;
    uint64_t table_size_bytes = (uint64_t)table_size * MSIX_TABLE_ENTRY_SIZE;
    uint64_t table_end = table_addr + table_size_bytes;

    if (table_end <= table_addr) return -1;
    if (table_end > bar->base + bar->size) { pr_warn("MSI-X: table beyond BAR"); return -1; }

    /* 32-bit non-PAE kernel: an identity map can only reach below 4 GiB. */
    uint64_t page_base = table_addr & ~0xFFFULL;
    uint64_t page_end  = (table_end + 0xFFF) & ~0xFFFULL;
    if (page_end > 0x100000000ULL) {
        pr_warn("MSI-X: table above 4 GiB not mappable");
        return -1;
    }

    for (uint64_t p = page_base; p < page_end; p += 0x1000)
        vmm_map(page_directory, (uint32_t)p, (uint32_t)p,
                PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    *table_out     = (volatile struct msix_table_entry *)(uintptr_t)(uint32_t)table_addr;
    *table_size_out = table_size;
    return 0;
}

int pci_msix_pba_map(pci_device_t *dev,
                     volatile uint32_t **pba_out)
{
    int cap_off = pci_msix_support(dev);
    if (!cap_off) return -1;

    uint32_t p_off = pci_read_config_dword(dev->bus, dev->dev, dev->fn, cap_off + 8);

    unsigned int pba_bir    = p_off & 0x7;
    uint32_t     pba_offset = p_off & ~0x7;

    if (pba_bir >= 6) return -1;

    pci_bar_t *bar = &dev->bars[pba_bir];
    if (bar->is_io || !bar->base) return -1;

    uint64_t pba_addr = bar->base + pba_offset;
    if (pba_addr >= 0x100000000ULL) return -1;
    uint64_t pba_page = pba_addr & ~0xFFFULL;

    vmm_map(page_directory, (uint32_t)pba_page, (uint32_t)pba_page,
            PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    *pba_out = (volatile uint32_t *)(uintptr_t)(uint32_t)pba_addr;
    return 0;
}

int pci_msix_enable(pci_device_t *dev, int vector,
                    volatile struct msix_table_entry *table,
                    unsigned int entry_idx)
{
    int cap_off = pci_msix_support(dev);
    if (!cap_off) return -1;

    /* MSG_CTRL is 16-bit at cap_off+2 (see pci_msix_table_map). */
    uint16_t msg_ctrl = pcidev_cfg_read16(dev, (uint16_t)(cap_off + 2));

    /* Idempotent: refuse a second enable on the same capability. */
    if (msg_ctrl & (1u << 15))
        return 0;

    unsigned int table_size = (msg_ctrl & 0x7FF) + 1;
    if (entry_idx >= table_size) return -1;

    /* Strict PCIe/xHCI ordering to avoid a spurious interrupt or Host System
     * Error during the switch-over:
     *   1. program address/data while the entry stays MASKED
     *   2. fence so the table writes are visible to the device
     *   3. switch off legacy INTx, then enable MSI-X globally
     *   4. fence, and only then unmask the vector
     * Unmasking before the global MSI-X enable lets the controller deliver an
     * interrupt against a half-configured function — fatal on some chipsets. */
    table[entry_idx].msg_addr_lo = 0xFEE00000u | (apic_lapic_id() << 12);
    table[entry_idx].msg_addr_hi = 0;
    table[entry_idx].msg_data    = vector;
    table[entry_idx].vector_ctrl = MSIX_VECTOR_CTRL_MASK;

    __asm__ volatile("sfence" ::: "memory");

    /* Disable legacy INTx so only MSI-X delivers. */
    uint32_t cmd = pci_read_config_dword(dev->bus, dev->dev, dev->fn, 0x04);
    cmd |= (1u << 10);
    pci_write_config_dword(dev->bus, dev->dev, dev->fn, 0x04, cmd);

    /* Now flip the function into MSI-X mode. */
    pcidev_cfg_write16(dev, (uint16_t)(cap_off + 2), msg_ctrl | (1u << 15));
    __asm__ volatile("sfence" ::: "memory");

    /* Controller is now globally in MSI-X mode — safe to unmask the entry. */
    table[entry_idx].vector_ctrl = 0;
    __asm__ volatile("sfence" ::: "memory");

    pr_info("MSI-X: enabled vector 0x%x entry %u (table 0x%x)",
            (unsigned)vector, (unsigned)entry_idx,
            (unsigned)((uint32_t)(uintptr_t)table));
    return 0;
}

int msix_used_vectors(void)
{
    int count = 0;
    for (unsigned int i = 0; i < MSIX_VECTOR_COUNT; i++)
        if (msix_vector_alloc[i]) count++;
    return count;
}
