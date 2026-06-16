#include "kernel.h"
#include "memory.h"
#include "acpi.h"
#include "apic.h"
#include "cact_acpi.h"
#include <string.h>

#define IA32_APIC_BASE      0x1B
#define APIC_ENABLE         (1u << 11)

#define LAPIC_SVR           0xF0
#define LAPIC_EOI           0xB0
#define LAPIC_SVR_ENABLE    0x100
#define LAPIC_SPURIOUS_VEC  0xFF

#define IOAPIC_IOREGSEL     0x00
#define IOAPIC_IOWIN        0x10
#define IOAPIC_VER          0x01
#define IOAPIC_REDIR_LO(i)  (0x10 + 2 * (i))
#define IOAPIC_REDIR_HI(i)  (0x10 + 2 * (i) + 1)

#define REDIR_MASKED        0x00010000u
#define REDIR_LOW_POL       0x00002000u  // Active low
#define REDIR_LEVEL         0x00008000u  // Level-triggered

#define APIC_MMIO_VADDR     0xFE000000u
#define IOAPIC_MMIO_VADDR   0xFE001000u

static volatile uint32_t *lapic = NULL;
static volatile uint32_t *ioapic_regsel = NULL;
static volatile uint32_t *ioapic_win = NULL;
static int apic_enabled = 0;
static unsigned int ioapic_max_redir = 0;
static unsigned int ioapic_global_irq_base = 0;
static unsigned int ioapic_id = 0;
static uint32_t lapic_base_addr = 0;
static uint32_t ioapic_base_addr = 0;
static uint32_t irq_override[16];

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static uint32_t ioapic_read(uint32_t reg)
{
    *ioapic_regsel = reg;
    return *ioapic_win;
}

static void ioapic_write(uint32_t reg, uint32_t val)
{
    *ioapic_regsel = reg;
    *ioapic_win = val;
}

static void ioapic_set_redir(unsigned int entry, uint8_t vector,
                              uint32_t flags, uint8_t dest)
{
    ioapic_write(IOAPIC_REDIR_HI(entry), (uint32_t)dest << 24);
    ioapic_write(IOAPIC_REDIR_LO(entry), vector | flags);
}

static void lapic_init(uint32_t lapic_base)
{
    vmm_map(get_current_pd(), APIC_MMIO_VADDR, lapic_base & ~0xFFF,
            PAGE_PRESENT | PAGE_RW | PAGE_PCD);
    lapic = (volatile uint32_t *)(APIC_MMIO_VADDR + (lapic_base & 0xFFF));

    lapic[0x320 / 4] = 0x00010000;
    lapic[0x350 / 4] = 0x00010000;
    lapic[0x360 / 4] = 0x00010000;
    lapic[0x370 / 4] = 0x00010000;

    uint64_t msr_val = rdmsr(IA32_APIC_BASE);
    msr_val &= ~0xFFF;
    msr_val |= (uint64_t)(uint32_t)lapic_base | APIC_ENABLE;
    wrmsr(IA32_APIC_BASE, msr_val);

    lapic[LAPIC_SVR / 4] = LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC;
}

int apic_init(void)
{
    ACPI_TABLE_MADT *madt = NULL;
    ACPI_STATUS status = AcpiGetTable("APIC", 1, (ACPI_TABLE_HEADER **)&madt);

    if (ACPI_FAILURE(status) || !madt) {
        klog(LOG_WARN, "APIC: MADT not found");
        return -1;
    }

    lapic_init(madt->Address);
    lapic_base_addr = madt->Address;

    uint32_t ioapic_base = 0;
    uint32_t global_irq_base = 0;
    uint8_t  ioapic_id_local = 0;

    for (int i = 0; i < 16; i++)
        irq_override[i] = i;

    uint8_t *entry = (uint8_t *)(madt + 1);
    uint8_t *end   = (uint8_t *)madt + madt->Header.Length;

    while (entry < end) {
        uint8_t type = entry[0];
        uint8_t len  = entry[1];

        if (type == 1) {
            ACPI_MADT_IO_APIC *ioapic = (ACPI_MADT_IO_APIC *)entry;
            ioapic_base      = ioapic->Address;
            global_irq_base  = ioapic->GlobalIrqBase;
            ioapic_id_local  = ioapic->Id;
        } else if (type == 2) {
            ACPI_MADT_INTERRUPT_OVERRIDE *ovr =
                (ACPI_MADT_INTERRUPT_OVERRIDE *)entry;
            if (ovr->SourceIrq < 16)
                irq_override[ovr->SourceIrq] = ovr->GlobalIrq;
        }
        entry += len;
    }

    ioapic_base_addr = ioapic_base;
    ioapic_id = ioapic_id_local;

    if (ioapic_base == 0) {
        klog(LOG_WARN, "IOAPIC: not found");
        return -1;
    }

    vmm_map(get_current_pd(), IOAPIC_MMIO_VADDR, ioapic_base & ~0xFFF,
            PAGE_PRESENT | PAGE_RW | PAGE_PCD);
    ioapic_regsel = (volatile uint32_t *)(IOAPIC_MMIO_VADDR + (ioapic_base & 0xFFF));
    ioapic_win    = (volatile uint32_t *)(IOAPIC_MMIO_VADDR + (ioapic_base & 0xFFF) + 0x10);

    ioapic_max_redir = (ioapic_read(IOAPIC_VER) >> 16) & 0xFF;
    ioapic_global_irq_base = global_irq_base;
    for (unsigned int i = 0; i <= ioapic_max_redir; i++)
        ioapic_set_redir(i, 0, REDIR_MASKED, 0);

    for (unsigned int i = 0; i < 16; i++) {
        if (i == 2) continue;
        unsigned int gsi = irq_override[i];
        if (gsi < global_irq_base) continue;
        unsigned int entry_idx = gsi - global_irq_base;
        if (entry_idx > ioapic_max_redir) continue;
        ioapic_set_redir(entry_idx, 0x20 + i, 0, 0);
    }

    // Program IOAPIC entries for PCI IRQs (GSI 16+, level-triggered active-low).
    // Use vectors 0xF0+ to avoid conflicting with MSI-X (0x30–0xEF).
    for (unsigned int i = 0; i <= ioapic_max_redir; i++) {
        unsigned int gsi = global_irq_base + i;
        if (gsi < 16) continue;
        if (gsi > 23) continue;
        ioapic_set_redir(i, 0xF0 + (i & 0x0F), REDIR_LEVEL | REDIR_LOW_POL, 0);
    }

    unsigned int timer_entry = irq_override[0];
    if (timer_entry < global_irq_base) timer_entry = 0;
    else timer_entry -= global_irq_base;
    if (timer_entry > ioapic_max_redir) timer_entry = 0;
    uint64_t period = hpet_get_freq() / 100;
    int hpet_ok = (period > 0 && hpet_start_periodic(timer_entry, period) == 0);

    apic_enabled = 1;

    {
        char buf[64]; char num[32];
        if (hpet_ok) {
            strcpy(buf, "APIC: HPET timer0 → IOAPIC entry ");
            itoa((int)timer_entry, num); strcat(buf, num);
            klog(LOG_OK, buf);
        } else {
            strcpy(buf, "APIC: PIT via IOAPIC entry ");
            itoa((int)timer_entry, num); strcat(buf, num);
            klog(LOG_WARN, buf);
        }
    }

    return 0;
}

bool apic_is_enabled(void) { return apic_enabled; }

void apic_eoi(void)
{
    if (apic_enabled && lapic)
        lapic[LAPIC_EOI / 4] = 0;
}

int apic_pci_vector(uint8_t irq_pin)
{
    if (!apic_enabled || irq_pin < 1 || irq_pin > 4)
        return -1;
    unsigned int gsi = irq_pin + 15;
    unsigned int entry_idx = gsi - ioapic_global_irq_base;
    if (entry_idx > ioapic_max_redir)
        return -1;
    return 0xF0 + (entry_idx & 0x0F);
}

uint32_t apic_lapic_base(void)       { return lapic_base_addr; }
uint32_t apic_lapic_id(void)
{
    uint32_t ebx, eax, ecx, edx;
    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0));
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> 24) & 0xFF;
}
bool     apic_ioapic_info(uint32_t *base, uint32_t *id, uint32_t *max_redir, uint32_t *gsi_base)
{
    if (!apic_enabled) return false;
    if (base)      *base      = ioapic_base_addr;
    if (id)        *id        = ioapic_id;
    if (max_redir) *max_redir = ioapic_max_redir;
    if (gsi_base)  *gsi_base  = ioapic_global_irq_base;
    return true;
}
int apic_irq_override(int isa_irq)
{
    if (!apic_enabled || isa_irq < 0 || isa_irq >= 16) return -1;
    return (int)irq_override[isa_irq];
}
