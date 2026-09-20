#include "msix.h"
#include "msi.h"
#include "msidev.h"
#include "pci.h"
#include "pcidev.h"
#include "pcie.h"
#include "kernel.h"
#include "memory.h"
#include "apic.h"
#include "klib.h"

/* Debug: freeze the machine once the MSI-X diagnostic below has been printed,
 * so the header dump stays on screen on real hardware.  Set to 0 when the
 * cause of a "capability not found" is known. */
#define MSIX_DIAG_HALT      1

/* One record per enabled MSI-X entry, so a resume can re-write the tables the
 * platform reset cleared (see msix_restore()). */
struct msix_enabled_entry {
    volatile struct msix_table_entry *table;
    unsigned int entry_idx;
    int          vector;
};
static struct msix_enabled_entry msix_enabled[MSIDEV_VECTOR_COUNT];

/* MMIO (BAR/MSI-X) mappings must live in the global kernel page directory,
 * never in whatever process PD happens to be active when the deferred driver
 * probe runs — otherwise the IRQ handler faults under a user CR3. */
extern uint32_t page_directory[1024];

/* Byte 0x34 is the head of the standard capability list.  Print the chain
 * entry by entry: "no 0x11 in the chain" is then a fact about the device
 * rather than a guess about the config-space accessor. */
static void msix_dump_chain(pci_device_t *dev, uint8_t head)
{
    if (head < 0x40 || head == 0xFF) {
        pr_warn("  %-11s : %02x:%02x.%u capability list absent (byte 0x34=0x%02x)\n",
                "msi-x", (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn,
                (unsigned)head);
        return;
    }

    pr_warn("  %-11s : %02x:%02x.%u capability chain from 0x%02x:\n",
            "msi-x", (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn,
            (unsigned)head);

    uint8_t p = head;
    for (int i = 0; i < 32 && p >= 0x40 && p != 0xFF; i++) {
        uint8_t id   = pcidev_cfg_read8(dev, p);
        uint8_t next = pcidev_cfg_read8(dev, (uint16_t)(p + 1));
        pr_info("  %-11s :   +0x%02x: id=0x%02x next=0x%02x%s\n",
                "msi-x", (unsigned)p, (unsigned)id, (unsigned)next,
                id == PCI_CAP_ID_MSIX ? "   <-- MSI-X" : "");
        p = next;
    }
}

/* Raw header tail — the fallback when the chain itself is unusable, so a
 * broken list pointer can be told apart from an unreadable config space. */
static void msix_dump_header(pci_device_t *dev, uint8_t head)
{
    pr_warn("  %-11s : %02x:%02x.%u byte 0x34=0x%02x — header 0x30..0x3f:\n",
            "msi-x", (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn,
            (unsigned)head);
    for (unsigned int off = 0x30; off < 0x40; off += 8) {
        pr_info("  %-11s :   %02x: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
                "msi-x", off,
                pcidev_cfg_read8(dev, (uint16_t)(off + 0)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 1)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 2)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 3)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 4)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 5)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 6)),
                pcidev_cfg_read8(dev, (uint16_t)(off + 7)));
    }
}

/* Last resort: some BIOSes/devices expose a broken or absent list pointer
 * while the MSI-X structure still sits in the capability region.  Structures
 * are DWORD-aligned, so step 4.  A stray 0x11 byte cannot false-positive: the
 * candidate must also name a memory BAR (the table lives in one), stay inside
 * the header's BIR field and point its NextPtr at a plausible offset. */
static int msix_brute_scan(pci_device_t *dev)
{
    for (unsigned int off = 0x40; off < 0x100; off += 4) {
        if (pcidev_cfg_read8(dev, (uint16_t)off) != PCI_CAP_ID_MSIX)
            continue;

        uint8_t  next     = pcidev_cfg_read8(dev, (uint16_t)(off + 1));
        uint16_t msg_ctrl = pcidev_cfg_read16(dev, (uint16_t)(off + 2));
        uint32_t tbl      = pcidev_cfg_read32(dev, (uint16_t)(off + 4));
        unsigned int bir  = tbl & 0x7;

        if (bir >= 6) {
            pr_info("  %-11s : 0x11 at 0x%x rejected (bir=%u out of range)\n",
                    "msi-x", off, bir);
            continue;
        }

        /* BARs are decoded for normal headers only; a bridge exposes none. */
        int bar_ok = !((dev->header_type & 0x7F) == PCI_HEADER_TYPE_NORMAL &&
                       (dev->bars[bir].is_io || !dev->bars[bir].base));

        if (bar_ok && (next == 0x00 || next >= 0x40)) {
            pr_info("  %-11s : capability found by scan at 0x%x (bir %u)\n",
                    "msi-x", off, bir);
            return (int)off;
        }

        /* Log the rejection: a silent skip is indistinguishable from "the
         * device has no MSI-X", which is exactly what sent this hunt wrong. */
        pr_info("  %-11s : 0x11 at 0x%x rejected (next=0x%02x ctrl=0x%04x bar=%d)\n",
                "msi-x", off, (unsigned)next, (unsigned)msg_ctrl, bar_ok);
    }
    return 0;
}

/* Stop the CPU with interrupts off.  Deliberately local: pulling acpi_halt()
 * in here would make this library depend on the ACPI stack for a debug stop. */
static void msix_diag_halt(void) __attribute__((noreturn));
static void msix_diag_halt(void)
{
    pr_warn("  %-11s : diagnostic halt — reset the machine to continue\n", "msi-x");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

int pci_msix_support(pci_device_t *dev)
{
    if (!dev) return 0;

    /* A function nobody enabled yet may not report its capability list. */
    msidev_ensure_enabled(dev);

    int cap = msidev_find_cap(dev, PCI_CAP_ID_MSIX);
    if (cap) return cap;

    /* Nothing found — print the chain, then the raw header if the chain
     * itself is unusable, so the next step is a fact and not a guess. */
    uint8_t cap_head = pcidev_cfg_read8(dev, 0x34);
    msix_dump_chain(dev, cap_head);
    if (cap_head < 0x40)
        msix_dump_header(dev, cap_head);

    int scanned = msix_brute_scan(dev);
    if (scanned) return scanned;

    /* Re-read the identity and byte 0x34 right here.  If both come back
     * correct while the walk found nothing, the config-space path is proven
     * good and the device simply has no MSI-X in its capability list.  Note
     * that a broken access path cannot produce 0x00: pcie_read8() returns
     * 0xFF for a bus outside every ECAM segment, and the legacy path returns
     * 0xFF for an absent device. */
    uint32_t id_now  = pcidev_cfg_read32(dev, 0x00);
    uint8_t  cap_now = pcidev_cfg_read8(dev, 0x34);
    pr_warn("  %-11s : capability not found (pcie=%d, byte0x34=0x%02x re-read=0x%02x, "
            "id=%04x:%04x re-read-id=%04x:%04x)\n",
            "msi-x", (int)pcie_is_available(), (unsigned)cap_head, (unsigned)cap_now,
            (unsigned)dev->vendor_id, (unsigned)dev->device_id,
            (unsigned)(id_now & 0xFFFF), (unsigned)(id_now >> 16));

#if MSIX_DIAG_HALT
    /* Freeze only when there is no fallback left.  With MSI present the caller
     * can still bring the device up, and halting here would prevent exactly
     * that — the diagnostic would defeat the feature it was built to guide. */
    if (!pci_msi_support(dev))
        msix_diag_halt();
#endif
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
    if (table_end > bar->base + bar->size) { pr_warn("  %-11s : table beyond BAR\n", "msi-x"); return -1; }

    /* 32-bit non-PAE kernel: an identity map can only reach below 4 GiB. */
    uint64_t page_base = table_addr & ~0xFFFULL;
    uint64_t page_end  = (table_end + 0xFFF) & ~0xFFFULL;
    if (page_end > 0x100000000ULL) {
        pr_warn("  %-11s : table above 4 GiB not mappable\n", "msi-x");
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
    table[entry_idx].msg_addr_lo = apic_msi_address();
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

    {
        unsigned int idx = (unsigned int)(vector - MSIDEV_VECTOR_BASE);
        if (idx < MSIDEV_VECTOR_COUNT) {
            msix_enabled[idx].table     = table;
            msix_enabled[idx].entry_idx = entry_idx;
            msix_enabled[idx].vector    = vector;
        }
    }

    pr_info("  %-11s : vec 0x%x enabled, entry %u (%02x:%02x.%u, table 0x%x)\n",
            "msi-x", (unsigned)vector, (unsigned)entry_idx,
            (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn,
            (unsigned)((uint32_t)(uintptr_t)table));
    return 0;
}

/* Write one MSI-X table entry: LAPIC address + vector, delivered as a fixed
 * (non-masked) interrupt.  MASK in vector_ctrl is bit 0. */
static void msix_program_entry(volatile struct msix_table_entry *e, int vector)
{
    e->vector_ctrl = MSIX_VECTOR_CTRL_MASK;
    __asm__ volatile("sfence" ::: "memory");
    e->msg_addr_lo = apic_msi_address();
    e->msg_addr_hi = 0;
    e->msg_data    = (uint32_t)vector;
    __asm__ volatile("sfence" ::: "memory");
    e->vector_ctrl = 0;
    __asm__ volatile("sfence" ::: "memory");
}

void msix_restore(void)
{
    uint32_t n = 0;

    for (unsigned int i = 0; i < MSIDEV_VECTOR_COUNT; i++) {
        if (!msix_enabled[i].table)
            continue;
        msix_program_entry(msix_enabled[i].table + msix_enabled[i].entry_idx,
                           msix_enabled[i].vector);
        n++;
    }

    if (n)
        pr_info("  %-11s : %u table entry(ies) reprogrammed\n",
                "msi-x", (unsigned)n);
}
