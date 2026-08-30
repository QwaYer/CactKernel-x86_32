#include "msi.h"
#include "pci.h"
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

void msix_init(void)
{
    memset(msix_handlers, 0, sizeof(msix_handlers));
    memset(msix_vector_alloc, 0, sizeof(msix_vector_alloc));
    // Reserve vector 0x80 (syscall gate) so MSI-X never overwrites it
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
    uint8_t cap_ptr = (uint8_t)(pci_read_config_dword(dev->bus, dev->dev, dev->fn, 0x34) & 0xFF);
    int iter = 0;
    while (cap_ptr && cap_ptr != 0xFF && iter++ < 48) {
        uint32_t cap = pci_read_config_dword(dev->bus, dev->dev, dev->fn, cap_ptr);
        uint8_t cap_id = (uint8_t)(cap & 0xFF);
        if (cap_id == PCI_CAP_ID_MSIX)
            return (int)cap_ptr;
        cap_ptr = (uint8_t)((cap >> 8) & 0xFF);
    }
    return 0;
}

int pci_msix_table_map(pci_device_t *dev,
                       volatile struct msix_table_entry **table_out,
                       uint32_t *table_size_out)
{
    int cap_off = pci_msix_support(dev);
    if (!cap_off) return -1;

    uint32_t mc   = pci_read_config_dword(dev->bus, dev->dev, dev->fn, cap_off + 2);
    uint32_t t_off = pci_read_config_dword(dev->bus, dev->dev, dev->fn, cap_off + 4);

    unsigned int table_bir = t_off & 0x7;
    uint32_t table_offset  = t_off & ~0x7;
    unsigned int table_size = ((mc >> 16) & 0x7FF) + 1;

    if (table_bir >= 6) return -1;

    pci_bar_t *bar = &dev->bars[table_bir];
    if (bar->is_io || !bar->base) return -1;

    uint32_t table_addr = bar->base + table_offset;
    uint32_t table_end;
    if (__builtin_uadd_overflow(table_addr, table_size * MSIX_TABLE_ENTRY_SIZE, &table_end)) {
        pr_warn("MSI-X: table end overflow"); return -1;
    }

    if (table_end > bar->base + bar->size) { pr_warn("MSI-X: table beyond BAR"); return -1; }

    uint32_t page_base = table_addr & ~0xFFF;
    uint32_t page_end  = (table_end + 0xFFF) & ~0xFFF;
    for (uint32_t p = page_base; p < page_end; p += 0x1000)
        vmm_map(get_current_pd(), p, p,
                PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    *table_out     = (volatile struct msix_table_entry *)table_addr;
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

    uint32_t pba_addr = bar->base + pba_offset;
    uint32_t pba_page = pba_addr & ~0xFFF;

    vmm_map(get_current_pd(), pba_page, pba_page,
            PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    *pba_out = (volatile uint32_t *)pba_addr;
    return 0;
}

int pci_msix_enable(pci_device_t *dev, int vector,
                    volatile struct msix_table_entry *table,
                    unsigned int entry_idx)
{
    int cap_off = pci_msix_support(dev);
    if (!cap_off) return -1;

    uint32_t mc = pci_read_config_dword(dev->bus, dev->dev, dev->fn, cap_off + 2);
    unsigned int table_size = ((mc >> 16) & 0x7FF) + 1;
    if (entry_idx >= table_size) return -1;

    table[entry_idx].msg_addr_lo = 0xFEE00000u;
    table[entry_idx].msg_addr_hi = 0;
    table[entry_idx].msg_data    = vector;
    table[entry_idx].vector_ctrl = 0;

    __asm__ volatile("sfence" ::: "memory");

    pci_write_config_dword(dev->bus, dev->dev, dev->fn, cap_off + 2,
                mc | (1u << 15));

    uint32_t cmd = pci_read_config_dword(dev->bus, dev->dev, dev->fn, 0x04);
    cmd &= ~(1u << 10);
    pci_write_config_dword(dev->bus, dev->dev, dev->fn, 0x04, cmd);

    return 0;
}

int msix_used_vectors(void)
{
    int count = 0;
    for (unsigned int i = 0; i < MSIX_VECTOR_COUNT; i++)
        if (msix_vector_alloc[i]) count++;
    return count;
}
