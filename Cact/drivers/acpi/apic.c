#include "kernel.h"
#include "klib.h"
#include "memory.h"
#include "acpi.h"
#include "apic.h"
#include "cact_acpi.h"
#include "idt.h"
#include "lapic_timer.h"

#define IA32_APIC_BASE      0x1B
#define APIC_ENABLE         (1u << 11)

#define LAPIC_TPR           0x80
#define LAPIC_SVR           0xF0
#define LAPIC_EOI           0xB0
#define LAPIC_SVR_ENABLE    0x100
#define LAPIC_SPURIOUS_VEC  0xFF

/* LVT register block.  These offsets are fixed by the xAPIC map and must not
 * be confused: 0x320 timer, 0x330 thermal, 0x340 perf, 0x350 LINT0,
 * 0x360 LINT1, 0x370 error (0x380 is the timer *initial count*). */
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_THERMAL   0x330
#define LAPIC_LVT_PERF      0x340
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_LVT_MASKED    0x000100FFu  /* masked, vector 0xFF (spurious gate) */

#define IOAPIC_IOREGSEL     0x00
#define IOAPIC_IOWIN        0x10
#define IOAPIC_VER          0x01
#define IOAPIC_REDIR_LO(i)  (0x10 + 2 * (i))
#define IOAPIC_REDIR_HI(i)  (0x10 + 2 * (i) + 1)

#define REDIR_MASKED        0x00010000u
#define REDIR_LOW_POL       0x00002000u  // Active low
#define REDIR_LEVEL         0x00008000u  // Level-triggered

/* Legacy 8254 channel 0 — last-resort tick source (see the watchdog). */
#define PIT_CH0_DATA        0x40
#define PIT_CMD             0x43
#define PIT_BASE_FREQ       1193182u

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

struct irq_override_info {
    uint32_t gsi;
    uint16_t flags;   /* MADT IntiFlags: [1:0] polarity, [3:2] trigger */
};
static struct irq_override_info irq_override[16];

/* Translate MADT interrupt-source-override flags to IOAPIC redirection flags.
 * Only explicit "active low" / "level" settings force bits; everything else
 * keeps the IOAPIC defaults (active-high edge). */
static uint32_t madt_flags_to_ioapic(uint16_t inti_flags)
{
    uint32_t flags = 0;
    if ((inti_flags & 0x3) == 0x3)          flags |= REDIR_LOW_POL;
    if (((inti_flags >> 2) & 0x3) == 0x3)   flags |= REDIR_LEVEL;
    return flags;
}

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

    /* Task Priority 0.  Firmware can leave a non-zero TPR behind, which masks
     * every interrupt of equal or lower priority — including the LVT timer —
     * even though the SVR says the APIC is enabled. */
    lapic[LAPIC_TPR / 4] = 0;

    /* Mask the LVT entries with a valid spurious vector (0xFF): any stray
     * delivery then targets the spurious-vector gate instead of tripping a
     * #GP and cascading into a triple fault.  The offsets are the real LVT
     * block (the old code wrote 0x380, which is the timer's initial count). */
    lapic[LAPIC_LVT_TIMER / 4]   = LAPIC_LVT_MASKED;
    lapic[LAPIC_LVT_THERMAL / 4] = LAPIC_LVT_MASKED;
    lapic[LAPIC_LVT_PERF / 4]    = LAPIC_LVT_MASKED;
    lapic[LAPIC_LVT_LINT0 / 4]   = LAPIC_LVT_MASKED;
    lapic[LAPIC_LVT_LINT1 / 4]   = LAPIC_LVT_MASKED;
    lapic[LAPIC_LVT_ERROR / 4]   = LAPIC_LVT_MASKED;

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
        pr_warn("  %-11s : MADT not found\n", "apic");
        return -1;
    }

    lapic_init(madt->Address);
    lapic_base_addr = madt->Address;

    uint32_t ioapic_base = 0;
    uint32_t global_irq_base = 0;
    uint8_t  ioapic_id_local = 0;

    for (int i = 0; i < 16; i++) {
        irq_override[i].gsi   = (uint32_t)i;
        irq_override[i].flags = 0;
    }

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
            if (ovr->SourceIrq < 16) {
                irq_override[ovr->SourceIrq].gsi   = ovr->GlobalIrq;
                irq_override[ovr->SourceIrq].flags = ovr->IntiFlags;
            }
        }
        entry += len;
    }

    ioapic_base_addr = ioapic_base;
    ioapic_id = ioapic_id_local;

    if (ioapic_base == 0) {
        pr_warn("  %-11s : IOAPIC not found\n", "apic");
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
        /* IRQ0 (legacy PIT) and IRQ2 (PIC cascade) stay masked: the LAPIC
         * timer now owns the scheduler tick, and leaving the PIT routed would
         * inject a second, differently-rated source into timer_isr(). */
        if (i == 0 || i == 2) continue;
        unsigned int gsi = irq_override[i].gsi;
        if (gsi < global_irq_base) continue;
        unsigned int entry_idx = gsi - global_irq_base;
        if (entry_idx > ioapic_max_redir) continue;
        ioapic_set_redir(entry_idx, 0x20 + i,
                         madt_flags_to_ioapic(irq_override[i].flags), 0);
    }

    // Program IOAPIC entries for PCI IRQs (GSI 16+, level-triggered active-low).
    // Use vectors 0xF0+ to avoid conflicting with MSI-X (0x30–0xEF).
    for (unsigned int i = 0; i <= ioapic_max_redir; i++) {
        unsigned int gsi = global_irq_base + i;
        if (gsi < 16) continue;
        if (gsi > 23) continue;
        ioapic_set_redir(i, 0xF0 + (i & 0x0F), REDIR_LEVEL | REDIR_LOW_POL, 0);
    }

    /*
     * ACPI SCI must be level-triggered.  Polarity is firmware-specific: most
     * machines use active-low, but some (this HP BIOS!) report "high level"
     * in the MADT interrupt-source override.  Honor the override for the SCI
     * GSI; only fall back to the ACPI-spec active-low default when the
     * firmware did not describe the line at all.
     */
    {
        uint16_t sci_gsi = AcpiGbl_FADT.SciInterrupt;
        if (sci_gsi != 0 && sci_gsi >= global_irq_base) {
            unsigned int entry_idx = sci_gsi - global_irq_base;
            if (entry_idx <= ioapic_max_redir) {
                uint32_t flags = REDIR_LEVEL;
                uint16_t madt_flags = 0;
                int have_override = 0;
                for (int src = 0; src < 16; src++) {
                    if (irq_override[src].gsi == sci_gsi) {
                        madt_flags = irq_override[src].flags;
                        have_override = 1;
                        break;
                    }
                }
                if (have_override)
                    flags |= madt_flags_to_ioapic(madt_flags);
                else
                    flags |= REDIR_LOW_POL;

                ioapic_set_redir(entry_idx, 0x20 + sci_gsi, flags, 0);
                pr_info("  %-11s : SCI on GSI %u (level/%s)\n", "apic",
                        (unsigned)sci_gsi,
                        (flags & REDIR_LOW_POL) ? "active-low" : "active-high");
            }
        }
    }

    /*
     * Scheduler tick.  The LAPIC timer is the only periodic interrupt
     * source: calibrate it against the ACPI PM timer and arm it at 100 Hz on
     * LAPIC_TIMER_VECTOR, which device_isrs.asm dispatches to the scheduler.
     * A failed calibration is retried by the boot watchdog in init().
     *
     * The IDT gate is (re)asserted here, after ACPI has had its chance to
     * install the SCI handler: AcpiOsInstallInterruptHandler() writes
     * 0x20 + FADT.SciInterrupt, and an SCI_INT of 0 would otherwise have
     * redirected the old 0x20 timer gate to the SCI stub, leaving the
     * scheduler with no ticks at all.
     */
    if (idt_gate_handler(LAPIC_TIMER_VECTOR) != (uint32_t)timer_isr)
        pr_warn("  %-11s : timer vector 0x%x was not the LAPIC ISR — restoring\n",
                "apic", (unsigned)LAPIC_TIMER_VECTOR);
    set_idt_gate(LAPIC_TIMER_VECTOR, (uint32_t)timer_isr);

    uint32_t per_ms = lapic_timer_calibrate();
    if (per_ms == 0)
        pr_crit("  %-11s : LAPIC timer calibration failed — no system tick\n", "apic");
    else
        lapic_timer_start_periodic(per_ms);

    apic_enabled = 1;

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

/* Expose the mapped LAPIC MMIO window (used by the LAPIC-timer quirk). */
volatile uint32_t *apic_lapic_regs(void) { return lapic; }

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
    return (int)irq_override[isa_irq].gsi;
}

/* Per-AP local APIC bring-up: ensure IA32_APIC_BASE is enabled and the SVR
 * points at a valid spurious vector with all LVT entries masked. */
void apic_ap_online(void)
{
    uint64_t msr_val = rdmsr(IA32_APIC_BASE);
    msr_val &= ~0xFFF;
    msr_val |= APIC_ENABLE;
    wrmsr(IA32_APIC_BASE, msr_val);

    if (lapic) {
        lapic[LAPIC_TPR / 4] = 0;
        lapic[LAPIC_LVT_TIMER / 4]   = LAPIC_LVT_MASKED;
        lapic[LAPIC_LVT_THERMAL / 4] = LAPIC_LVT_MASKED;
        lapic[LAPIC_LVT_PERF / 4]    = LAPIC_LVT_MASKED;
        lapic[LAPIC_LVT_LINT0 / 4]   = LAPIC_LVT_MASKED;
        lapic[LAPIC_LVT_LINT1 / 4]   = LAPIC_LVT_MASKED;
        lapic[LAPIC_LVT_ERROR / 4]   = LAPIC_LVT_MASKED;
        lapic[LAPIC_SVR / 4] = LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC;
    }
}

static int apic_icr_busy(void)
{
    if (!lapic) return 1;
    return (lapic[0x300 / 4] & (1u << 12)) != 0;
}

static void apic_icr_send(uint32_t dest_lapic, uint32_t icrlo)
{
    if (!lapic) return;
    for (int i = 0; i < 100000 && apic_icr_busy(); i++)
        __asm__ __volatile__("pause");
    lapic[0x310 / 4] = (dest_lapic & 0xFFu) << 24;   /* ICR high */
    lapic[0x300 / 4] = icrlo;                        /* ICR low  */
}

/* Send INIT to a target APIC id (level-triggered assert, delivery mode INIT). */
void apic_send_init_ipi(uint32_t dest_lapic)
{
    apic_icr_send(dest_lapic, 0x0000C500u);
}

/* Send SIPI to a target APIC id; vector = (startup page address >> 12). */
void apic_send_sipi(uint32_t dest_lapic, uint32_t vector)
{
    apic_icr_send(dest_lapic, 0x00000600u | (vector & 0xFFu));
}
