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
#define APIC_X2APIC_ENABLE  (1u << 10)
#define APIC_BASE_ADDR_MASK 0xFFFFF000ull

/* x2APIC moves the LAPIC register file into MSRs: the register at byte offset
 * `off` of the xAPIC map lives at MSR 0x800 + off/16.  The ICR is the one
 * register that changes shape — a single 64-bit MSR with the destination in
 * the high half, instead of the two 32-bit MMIO registers. */
#define X2APIC_MSR(off)     (0x800u + ((off) >> 4))
#define X2APIC_ICR          0x830

#define LAPIC_ID            0x20
#define LAPIC_TPR           0x80
#define LAPIC_PPR           0xA0
#define LAPIC_SVR           0xF0
#define LAPIC_EOI           0xB0
#define LAPIC_ISR           0x100   /* 8 x 32-bit words: vectors 0..255 */
#define LAPIC_IRR           0x200
#define LAPIC_ICR           0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_SVR_ENABLE    0x100
#define LAPIC_SPURIOUS_VEC  0xFF
#define ICR_DELIVERY_PENDING (1u << 12)

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

/* Timer registers, repeated here so the state dump can read them back. */
#define LAPIC_TIMER_DIV     0x3E0
#define LAPIC_TIMER_INITCNT 0x380
#define LAPIC_TIMER_CURCNT  0x390

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
static int apic_x2apic = 0;
/* Why the LAPIC is in the mode it is — surfaced in dmesg and /proc/apic so a
 * silent fall back to xAPIC is never a mystery. */
static const char *x2apic_note = "not probed";
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

/* LAPIC register access, mode-agnostic.  `reg` is the byte offset in the xAPIC
 * register map (0x80 TPR, 0xF0 SVR, ...) and is translated to the corresponding
 * MSR in x2APIC mode, so callers never have to know which mode is active. */
uint32_t apic_lapic_read(uint32_t reg)
{
    if (apic_x2apic)
        return (uint32_t)rdmsr(X2APIC_MSR(reg));
    if (!lapic)
        return 0;
    return lapic[reg / 4];
}

void apic_lapic_write(uint32_t reg, uint32_t val)
{
    if (apic_x2apic) {
        wrmsr(X2APIC_MSR(reg), val);
        return;
    }
    if (!lapic)
        return;
    lapic[reg / 4] = val;
}

/* Highest set vector across the eight ISR (or IRR) words, or -1 if none. */
static int lapic_pending_vector(uint32_t base)
{
    for (int w = 7; w >= 0; w--) {
        uint32_t v = apic_lapic_read(base + (uint32_t)w * 0x10);
        if (!v)
            continue;
        for (int b = 31; b >= 0; b--)
            if (v & (1u << b))
                return w * 32 + b;
    }
    return -1;
}

/* Retire every in-service entry.  Firmware — or any delivery whose EOI never
 * landed — leaves an ISR bit set, and while it is set PPR sits at that
 * vector's priority, so every interrupt of the same or lower class (the
 * LAPIC timer included) waits in IRR forever.  One EOI clears only the
 * highest in-service bit, so a single write is not enough when several are
 * stuck; hence the bounded loop with a report of what was retired. */
void apic_clear_in_service(void)
{
    if (!apic_lapic_ready())
        return;

    int retired = 0;
    for (int i = 0; i < 256; i++) {
        int vec = lapic_pending_vector(LAPIC_ISR);
        if (vec < 0)
            break;
        if (retired < 8)
            pr_warn("  %-11s : stale in-service vector 0x%x — retiring it\n",
                    "apic", (unsigned)vec);
        apic_lapic_write(LAPIC_EOI, 0);
        retired++;
    }
    if (retired > 8)
        pr_warn("  %-11s : %d stale in-service entries retired\n",
                "apic", retired);
}

/* Handler for the spurious vector (SVR[7:0], 0xFF).
 *
 * The specification says a spurious delivery does not set an in-service bit,
 * so a bare return is the correct handler — and that is what this used to be.
 * This hardware sets the bit anyway.  Left in service it pins PPR at 0xF0, the
 * highest priority class, which blocks every interrupt of class <= F: the
 * LAPIC timer (0xFE) and every MSI-X vector (0x30-0xEF) with it.  The machine
 * stays alive on polling alone — boot completes, PCI enumeration and xHCI
 * device bring-up print success, MSI-X "enables" — while the timer never
 * ticks and a USB keyboard never delivers a report.
 *
 * So retire the bit here, where it was set.  A spurious interrupt that keeps
 * arriving is a finding in itself: report the first few with the state that
 * explains them, and stay quiet afterwards instead of flooding the log from
 * interrupt context. */
void spurious_apic_handler(void)
{
    static uint32_t spurious_count;

    spurious_count++;
    if (spurious_count <= 8) {
        pr_warn("  %-11s : spurious interrupt #%u (isr=%d irr=%d ppr=0x%x) — "
                "retiring in-service bit\n",
                "apic", (unsigned)spurious_count,
                lapic_pending_vector(LAPIC_ISR),
                lapic_pending_vector(LAPIC_IRR),
                (unsigned)apic_lapic_read(LAPIC_PPR));
    }

    apic_clear_in_service();
}

/* Program the mandatory LAPIC state: TPR 0, all LVT entries masked with a
 * spurious vector, and the APIC enabled through the SVR. */
static void lapic_common_setup(void)
{
    /* Task Priority 0.  Firmware can leave a non-zero TPR behind, which masks
     * every interrupt of equal or lower priority — including the LVT timer —
     * even though the SVR says the APIC is enabled. */
    apic_lapic_write(LAPIC_TPR, 0);

    /* Mask the LVT entries with a valid spurious vector (0xFF): any stray
     * delivery then targets the spurious-vector gate instead of tripping a
     * #GP and cascading into a triple fault.  The offsets are the real LVT
     * block (the old code wrote 0x380, which is the timer's initial count). */
    apic_lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    apic_lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    apic_lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    apic_lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_MASKED);
    apic_lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_MASKED);
    apic_lapic_write(LAPIC_LVT_ERROR,   LAPIC_LVT_MASKED);

    apic_lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);

    /* A stale in-service entry — firmware's, or any delivery whose EOI never
     * landed — holds PPR up and blocks the timer's priority class forever. */
    apic_clear_in_service();

    /* TPR and SVR gate every local delivery, and firmware is free to leave
     * them dirty: a non-zero TPR masks the timer's priority class outright,
     * and without SVR[8] the LAPIC ignores local interrupts entirely.  Read
     * back what actually landed, so a "tick armed but silent" report points
     * at the real culprit instead of the timer code. */
    uint32_t tpr = apic_lapic_read(LAPIC_TPR);
    if ((tpr & 0xFFu) != 0)
        pr_warn("  %-11s : TPR reads 0x%x after clearing — priority class %u "
                "masks lower-priority interrupts\n",
                "apic", (unsigned)tpr, (unsigned)(tpr >> 4));

    uint32_t svr = apic_lapic_read(LAPIC_SVR);
    if (!(svr & LAPIC_SVR_ENABLE))
        pr_warn("  %-11s : SVR reads 0x%x — software-enable bit clear, "
                "LAPIC will not deliver\n", "apic", (unsigned)svr);
}

/* Bring up the BSP local APIC, preferring x2APIC when the CPU offers it.
 *
 * The x2APIC transition is one-way until reset, so it is taken only after a
 * successful read-back of IA32_APIC_BASE[10] — a CPU (or hypervisor) that
 * advertises the CPUID bit without implementing the MSRs leaves us in xAPIC
 * mode with the MMIO window intact.  Once in x2APIC mode the MMIO window is
 * neither mapped nor touched: the base address field is ignored by hardware
 * and every access goes through an MSR. */
static void lapic_init(uint32_t lapic_base)
{
    uint64_t msr_val = rdmsr(IA32_APIC_BASE);

    if (msr_val & APIC_X2APIC_ENABLE) {
        /* Firmware already switched: MMIO access would not reach the LAPIC. */
        apic_x2apic = 1;
        x2apic_note = "already enabled by firmware";
    } else if (cpu_has_x2apic()) {
        apic_x2apic = 1;
        wrmsr(IA32_APIC_BASE, msr_val | APIC_ENABLE | APIC_X2APIC_ENABLE);
        if (!(rdmsr(IA32_APIC_BASE) & APIC_X2APIC_ENABLE)) {
            pr_warn("  %-11s : x2APIC enable did not take — staying on xAPIC "
                    "(IA32_APIC_BASE[10] would not set)\n", "apic");
            apic_x2apic = 0;
            x2apic_note = "IA32_APIC_BASE[10] would not set";
        } else {
            x2apic_note = "enabled by kernel";
        }
    } else {
        x2apic_note = "CPUID.01H:ECX[21] not set";
    }

    if (apic_x2apic) {
        lapic_base_addr = 0xFEE00000u;   /* architectural LAPIC address */
    } else {
        /* In xAPIC mode the base comes from the MADT; a MADT that omits it
         * (legal when x2APIC is in use) must not make us map page zero. */
        if (lapic_base == 0)
            lapic_base = 0xFEE00000u;

        vmm_map(get_current_pd(), APIC_MMIO_VADDR, lapic_base & ~0xFFF,
                PAGE_PRESENT | PAGE_RW | PAGE_PCD);
        lapic = (volatile uint32_t *)(APIC_MMIO_VADDR + (lapic_base & 0xFFF));

        msr_val = (msr_val & ~APIC_BASE_ADDR_MASK) |
                  (uint64_t)(lapic_base & APIC_BASE_ADDR_MASK) | APIC_ENABLE;
        wrmsr(IA32_APIC_BASE, msr_val);

        lapic_base_addr = lapic_base;
    }

    lapic_common_setup();

    pr_info("  %-11s : %s, %s, LAPIC ID %u\n", "apic",
            apic_x2apic ? "x2APIC (MSR access)" : "xAPIC (MMIO)",
            apic_x2apic_note(), (unsigned)apic_lapic_id());
    /* Unmistakable build marker: when a fix "does not work" on hardware, the
     * first question is whether the image actually contains it. */
    pr_info("  %-11s : diag v4 (ISR drain + vector numbers)\n", "apic");
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
     * source: arm it at 100 Hz on LAPIC_TIMER_VECTOR, which device_isrs.asm
     * dispatches to the scheduler.  The tick rate is calibrated against the
     * ACPI PM timer once, at boot; an S3 resume re-arms from that saved value
     * because the PM-timer reference does not come back after the sleep (and
     * polling it for a calibration window would stall for minutes).  A failed
     * calibration is retried by the boot watchdog in init().
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

    uint32_t per_ms = lapic_timer_ticks_per_ms();
    if (per_ms == 0)
        per_ms = lapic_timer_calibrate();   /* first bring-up only */
    if (per_ms == 0)
        pr_crit("  %-11s : LAPIC timer calibration failed — no system tick\n", "apic");
    else
        lapic_timer_start_periodic(per_ms);

    apic_enabled = 1;

    return 0;
}

bool apic_is_enabled(void) { return apic_enabled; }

bool apic_x2apic_mode(void) { return apic_x2apic != 0; }

const char *apic_x2apic_note(void) { return x2apic_note; }

/* True once the LAPIC can be talked to — either mode qualifies.  Distinct from
 * apic_is_enabled(), which only flips after the whole APIC/IOAPIC bring-up. */
bool apic_lapic_ready(void) { return apic_x2apic != 0 || lapic != NULL; }

/* Retire the in-service entry of whichever interrupt is being handled.
 *
 * Gated on readiness, not on apic_enabled: the flag only flips at the very end
 * of apic_init(), after the IOAPIC redirections and the timer LVT are unmasked,
 * so an interrupt delivered in that window runs its handler and then drops its
 * EOI — and the vector stays in service, pinning PPR for good.  Writing an EOI
 * is valid for as long as the LAPIC can be addressed, and is harmless when
 * nothing is in service. */
void apic_eoi(void)
{
    if (apic_lapic_ready())
        apic_lapic_write(LAPIC_EOI, 0);
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
    /* x2APIC IDs are 32 bits and CPUID.01H:EBX[31:24] is documented as stale
     * in that mode, so read the LAPIC ID register itself. */
    if (apic_x2apic)
        return apic_lapic_read(LAPIC_ID);

    uint32_t ebx, eax, ecx, edx;
    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0));
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> 24) & 0xFF;
}

/* MSI/MSI-X message address for this logical processor.  The destination APIC
 * ID lives in address bits 19:12 — eight bits — and that field does not widen
 * in x2APIC mode: the message format is unchanged, so without interrupt
 * remapping (VT-d/AMD-Vi, which this kernel does not program) a message can
 * only reach an ID below 256.  Mask explicitly: an unmasked id would spill
 * into the reserved bits above 19 and turn the store into an unclaimed-MMIO
 * write instead of a delivery to some CPU. */
uint32_t apic_msi_address(void)
{
    uint32_t id = apic_lapic_id();

    if (id > 0xFFu) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            pr_warn("  %-11s : LAPIC ID %u exceeds MSI's 8-bit destination — "
                    "messages go to ID %u (no interrupt remapping)\n",
                    "apic", (unsigned)id, (unsigned)(id & 0xFFu));
        }
    }

    return 0xFEE00000u | ((id & 0xFFu) << 12);
}

/* Dump every LAPIC register that can gate local delivery, plus a one-line
 * verdict for the usual culprits.  Called from the boot watchdog when the
 * scheduler tick never arrives: "LAPIC timer dead" names the symptom, not the
 * cause, and the cause lives in exactly these bits. */
void apic_dump_state(const char *tag)
{
    if (!apic_lapic_ready()) {
        pr_warn("  %-11s : %s: LAPIC not reachable\n", "apic", tag);
        return;
    }

    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(flags));

    uint64_t base = rdmsr(IA32_APIC_BASE);
    uint32_t tpr  = apic_lapic_read(LAPIC_TPR);
    uint32_t ppr  = apic_lapic_read(LAPIC_PPR);
    uint32_t svr  = apic_lapic_read(LAPIC_SVR);
    uint32_t lvt  = apic_lapic_read(LAPIC_LVT_TIMER);
    uint32_t init = apic_lapic_read(LAPIC_TIMER_INITCNT);
    uint32_t cur  = apic_lapic_read(LAPIC_TIMER_CURCNT);
    uint32_t tdcr = apic_lapic_read(LAPIC_TIMER_DIV);
    uint32_t gate = idt_gate_handler(LAPIC_TIMER_VECTOR);

    /* Sample the counter twice: a single reading (or two dumps 2 s apart)
     * cannot tell "counting" from "frozen", because the counter wraps once per
     * period.  ~2 ms of pause is enough to see any live counter move. */
    for (volatile uint32_t spin = 0; spin < 200000u; spin++)
        __asm__ __volatile__("pause");
    uint32_t cur2 = apic_lapic_read(LAPIC_TIMER_CURCNT);

    /* Highest in-service and pending vectors, so a stuck entry names itself
     * instead of only showing up as a raised PPR.  -1 means "none". */
    int in_service = lapic_pending_vector(LAPIC_ISR);
    int pending    = lapic_pending_vector(LAPIC_IRR);

    pr_info("  %-11s : %s: %s, APIC_BASE=0x%llx EN=%u X2=%u IF=%u\n", "apic", tag,
            apic_x2apic_mode() ? "x2APIC" : "xAPIC",
            (unsigned long long)base,
            (unsigned)((base >> 11) & 1u), (unsigned)((base >> 10) & 1u),
            (unsigned)((flags >> 9) & 1u));
    pr_info("  %-11s :   TPR=0x%x PPR=0x%x SVR=0x%x LVT=0x%x mask=%u periodic=%u\n",
            "apic", (unsigned)tpr, (unsigned)ppr, (unsigned)svr, (unsigned)lvt,
            (unsigned)((lvt >> 16) & 1u), (unsigned)((lvt >> 17) & 1u));
    pr_info("  %-11s :   INIT=0x%x CUR=0x%x->0x%x TDCR=0x%x isr=%d irr=%d gate=0x%x\n",
            "apic", (unsigned)init, (unsigned)cur, (unsigned)cur2, (unsigned)tdcr,
            in_service, pending, (unsigned)gate);

    if (!(base & APIC_ENABLE))
        pr_warn("  %-11s :   -> APIC_BASE[11] clear: LAPIC globally disabled\n", "apic");
    if (!((flags >> 9) & 1u))
        pr_warn("  %-11s :   -> EFLAGS.IF clear: interrupts globally disabled\n", "apic");
    if (!(svr & LAPIC_SVR_ENABLE))
        pr_warn("  %-11s :   -> SVR[8] clear: LAPIC ignores local interrupts\n", "apic");
    if (lvt & 0x10000u)
        pr_warn("  %-11s :   -> LVT mask set: tick gated at the source\n", "apic");
    if ((lvt & 0xFFu) != LAPIC_TIMER_VECTOR)
        pr_warn("  %-11s :   -> LVT vector 0x%x, expected 0x%x\n", "apic",
                (unsigned)(lvt & 0xFFu), (unsigned)LAPIC_TIMER_VECTOR);
    if (init == 0)
        pr_warn("  %-11s :   -> INIT count 0: timer never started\n", "apic");
    else if (cur == cur2)
        pr_warn("  %-11s :   -> counter frozen at 0x%x (INIT=0x%x): LAPIC timer "
                "is not counting\n", "apic", (unsigned)cur, (unsigned)init);
    if (in_service >= 0)
        pr_warn("  %-11s :   -> in-service vector 0x%x was never EOI'd (PPR=0x%x): "
                "the tick waits in IRR\n", "apic",
                (unsigned)in_service, (unsigned)ppr);
    else if (ppr > tpr)
        pr_warn("  %-11s :   -> PPR 0x%x > TPR 0x%x with no ISR bit set\n",
                "apic", (unsigned)ppr, (unsigned)tpr);
    else if (pending == LAPIC_TIMER_VECTOR)
        pr_info("  %-11s :   -> tick sits in IRR: the timer fires, delivery is "
                "what is blocked\n", "apic");
    if (gate != (uint32_t)timer_isr)
        pr_warn("  %-11s :   -> IDT gate 0x%x is not timer_isr 0x%x\n", "apic",
                (unsigned)gate, (unsigned)(uint32_t)timer_isr);
}

/* One-line live snapshot for the boot watchdog: is the counter moving, is the
 * tick pending (IRR) and delivered (ISR), and is anything gating it
 * (TPR/PPR/SVR/LVT/IF).  Printed every 250 ms while the tick is missing, so a
 * silent timer death reads as a timeline instead of two snapshots. */
void apic_probe(const char *tag, unsigned ms, unsigned ticks)
{
    if (!apic_lapic_ready()) {
        pr_warn("  %-11s : %s t=%ums ticks=%u: LAPIC not reachable\n",
                "apic", tag, ms, ticks);
        return;
    }

    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(flags));

    uint32_t tpr = apic_lapic_read(LAPIC_TPR);
    uint32_t ppr = apic_lapic_read(LAPIC_PPR);
    uint32_t svr = apic_lapic_read(LAPIC_SVR);
    uint32_t lvt = apic_lapic_read(LAPIC_LVT_TIMER);
    uint32_t cur0 = apic_lapic_read(LAPIC_TIMER_CURCNT);
    int in_service = lapic_pending_vector(LAPIC_ISR);   /* highest, -1 = none */
    int pending    = lapic_pending_vector(LAPIC_IRR);

    /* ~2 ms of pause: enough for any live counter to move. */
    for (volatile uint32_t spin = 0; spin < 200000u; spin++)
        __asm__ __volatile__("pause");
    uint32_t cur1 = apic_lapic_read(LAPIC_TIMER_CURCNT);

    pr_info("  %-11s : %s t=%ums ticks=%u cur=%s isr=%d irr=%d tpr=0x%x ppr=0x%x "
            "svr=0x%x lvt=0x%x if=%u\n", "apic", tag, ms, ticks,
            (cur1 != cur0) ? "run" : "FROZEN", in_service, pending,
            (unsigned)tpr, (unsigned)ppr, (unsigned)svr, (unsigned)lvt,
            (unsigned)((flags >> 9) & 1u));
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

    if (apic_x2apic) {
        /* IA32_APIC_BASE is per logical processor: an AP starts in xAPIC mode
         * even when the BSP has already switched, so it must switch here too
         * before the first MSR-addressed register write below. */
        wrmsr(IA32_APIC_BASE, msr_val | APIC_ENABLE | APIC_X2APIC_ENABLE);
    } else {
        msr_val = (msr_val & ~APIC_BASE_ADDR_MASK) |
                  (uint64_t)(lapic_base_addr & APIC_BASE_ADDR_MASK) | APIC_ENABLE;
        wrmsr(IA32_APIC_BASE, msr_val);
    }

    lapic_common_setup();
}

static int apic_icr_busy(void)
{
    return (apic_lapic_read(LAPIC_ICR) & ICR_DELIVERY_PENDING) != 0;
}

static void apic_icr_send(uint32_t dest_lapic, uint32_t icrlo)
{
    if (!apic_lapic_ready())
        return;

    for (int i = 0; i < 100000 && apic_icr_busy(); i++)
        __asm__ __volatile__("pause");

    if (apic_x2apic) {
        /* One 64-bit MSR: destination in the high half, delivery status still
         * bit 12 of the low half. */
        wrmsr(X2APIC_ICR, ((uint64_t)dest_lapic << 32) | icrlo);
        return;
    }

    apic_lapic_write(LAPIC_ICR_HIGH, (dest_lapic & 0xFFu) << 24);
    apic_lapic_write(LAPIC_ICR, icrlo);
}

/* Fixed-delivery IPI to one destination (physical mode).  Returns 0 when the
 * interrupt was issued, -1 when the LAPIC is not up. */
int apic_send_ipi(uint32_t dest_lapic, uint32_t vector)
{
    if (!apic_lapic_ready())
        return -1;
    apic_icr_send(dest_lapic, vector & 0xFFu);
    return 0;
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
