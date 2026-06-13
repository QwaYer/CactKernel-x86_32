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

#define APIC_MMIO_VADDR     0xFE000000u
#define IOAPIC_MMIO_VADDR   0xFE001000u

static volatile uint32_t *lapic = NULL;
static volatile uint32_t *ioapic_regsel = NULL;
static volatile uint32_t *ioapic_win = NULL;
static int apic_enabled = 0;

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

    uint32_t ioapic_base = 0;
    uint32_t global_irq_base = 0;
    uint8_t  ioapic_id = 0;
    uint32_t irq_override[16];

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
            ioapic_id        = ioapic->Id;
        } else if (type == 2) {
            ACPI_MADT_INTERRUPT_OVERRIDE *ovr =
                (ACPI_MADT_INTERRUPT_OVERRIDE *)entry;
            if (ovr->SourceIrq < 16)
                irq_override[ovr->SourceIrq] = ovr->GlobalIrq;
        }
        entry += len;
    }

    if (ioapic_base == 0) {
        klog(LOG_WARN, "IOAPIC: not found");
        return -1;
    }

    vmm_map(get_current_pd(), IOAPIC_MMIO_VADDR, ioapic_base & ~0xFFF,
            PAGE_PRESENT | PAGE_RW | PAGE_PCD);
    ioapic_regsel = (volatile uint32_t *)(IOAPIC_MMIO_VADDR + (ioapic_base & 0xFFF));
    ioapic_win    = (volatile uint32_t *)(IOAPIC_MMIO_VADDR + (ioapic_base & 0xFFF) + 0x10);

    unsigned int max_redir = (ioapic_read(IOAPIC_VER) >> 16) & 0xFF;
    for (unsigned int i = 0; i <= max_redir; i++)
        ioapic_set_redir(i, 0, REDIR_MASKED, 0);

    for (unsigned int i = 0; i < 16; i++) {
        if (i == 2) continue;
        unsigned int gsi = irq_override[i];
        unsigned int entry_idx = gsi - global_irq_base;
        ioapic_set_redir(entry_idx, 0x20 + i, 0, 0);
    }

    port_byte_out(0x21, 0xFF);
    port_byte_out(0xA1, 0xFF);
    apic_enabled = 1;

    {
        char buf[64]; char num[32];
        strcpy(buf, "APIC: 15 IRQs routed, PIT via IOAPIC entry ");
        itoa((int)(irq_override[0] - global_irq_base), num); strcat(buf, num);
        klog(LOG_OK, buf);
    }

    return 0;
}

bool apic_is_enabled(void) { return apic_enabled; }

void apic_eoi(void)
{
    if (apic_enabled && lapic)
        lapic[LAPIC_EOI / 4] = 0;
}
