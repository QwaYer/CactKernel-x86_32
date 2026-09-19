/*
 * power.c — ACPI system power transitions.
 *
 * Owns the kernel's powerdown and suspend paths:
 *   - S5 soft-off    (poweroff)   -> AcpiEnterSleepState(S5)
 *   - reset register (reboot)     -> AcpiReset()
 *   - S3 suspend     (sleep)      -> AcpiEnterSleepState(S3) + trampoline resume
 *   - halt                        -> stop the CPU with interrupts off
 *
 * Every destructive path is layered: the ACPI/ACPICA route first, then a
 * direct PM1a/PM1b register write from \_Sx, then the QEMU/Bochs PM1a port
 * convention, then a CPU-level fallback.  A machine with a broken or absent
 * FADT therefore still powers off / resets instead of hanging forever.
 *
 * S3 resume.  On S3 wake the platform resets the machine; the firmware does an
 * S3 resume and jumps to the FACS waking vector in real mode.  We therefore
 * stage a small real-mode trampoline (wake_trampoline.asm) in conventional
 * memory, point the FACS vector at it, and save our resume context before
 * entering S3.  The trampoline switches to protected mode, re-enables paging
 * with the kernel page directory and calls acpi_resume_entry(), which
 * reinitialises the CPU and hands control back here.
 *
 * The helpers that end the system do not return; acpi_suspend() returns 0 once
 * the platform has resumed.
 */

#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"
#include "acpi.h"
#include "cact_acpi.h"
#include "ktime.h"
#include "gdt.h"
#include "idt.h"
#include "apic.h"
#include "cpudev.h"
#include "pat.h"
#include "mtrr.h"
#include "fb.h"

/* Legacy PM1 control register layout (ACPI 1.0): SLP_TYP occupies bits 10-12,
 * SLP_EN is bit 13 of PM1x_CNT. */
#define PM1_SLP_TYP_SHIFT   10
#define PM1_SLP_EN          (1u << 13)

/* QEMU's ACPI PM1a control block for the q35/i440fx machine types, plus the
 * older Bochs-derived port some PC firmware still answers on. */
#define QEMU_PM1A_CNT       0x604
#define BOCHS_PM1A_CNT      0xB004

/* 8042 keyboard controller: pulse the reset line when no ACPI reset exists. */
#define KBC_STATUS_PORT     0x64
#define KBC_IBF             0x02
#define KBC_RESET_CMD       0xFE

/* S3-resume trampoline placement (conventional memory; the SMP AP trampoline
 * owns 0x8000, so this one sits at 0xA000).  Offsets must match
 * wake_trampoline.asm (INFO_BASE 0xAF00). */
#define WAKE_TRAMP_ADDR     0xA000u
#define WAKE_INFO_CR3       (WAKE_TRAMP_ADDR + 0x0F00u)
#define WAKE_INFO_STACK     (WAKE_TRAMP_ADDR + 0x0F04u)
#define WAKE_INFO_ENTRY     (WAKE_TRAMP_ADDR + 0x0F08u)

extern const uint8_t _binary_cact_wake_bin_start[];
extern const uint8_t _binary_cact_wake_bin_end[];

static int pm_ready;         /* ACPICA namespace + FADT usable            */
static uint32_t s_state_mask;/* bit Sx set when the \_Sx object is present */

/* ---------------------------------------------------------------------------
 * S3 resume state
 * --------------------------------------------------------------------------- */

struct acpi_resume_ctx {
    uint32_t cr3, esp, ebp, ebx, esi, edi, eip;
};

/* Written by wake_entry.asm (acpi_do_suspend) and read by acpi_ctx_restore. */
struct acpi_resume_ctx g_resume;
static uint32_t              g_suspend_state;
static uint8_t               acpi_resume_stack[16384] __attribute__((aligned(16)));

extern int acpi_do_suspend(uint32_t state);

static void acpi_ctx_restore(void) __attribute__((noreturn));

void acpi_resume_entry(void) __attribute__((noreturn));

static uint32_t read_cr3(void)
{
    uint32_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* Restore the saved context and jump back into acpi_suspend().  Every value is
 * loaded into registers while the kernel page directory is still active;
 * CR3 is switched last so nothing is read through the task's directory. */
static void acpi_ctx_restore(void)
{
    __asm__ volatile (
        "movl %0, %%ebx\n\t"
        "movl %1, %%esi\n\t"
        "movl %2, %%edi\n\t"
        "movl %3, %%ebp\n\t"
        "movl %4, %%eax\n\t"
        "movl %5, %%edx\n\t"
        "movl %6, %%esp\n\t"
        "movl %%edx, %%cr3\n\t"
        "jmp *%%eax\n\t"
        :: "m"(g_resume.ebx), "m"(g_resume.esi), "m"(g_resume.edi),
           "m"(g_resume.ebp), "m"(g_resume.eip), "m"(g_resume.cr3),
           "m"(g_resume.esp)
        : "eax", "ebx", "edx", "esi", "edi", "ebp", "memory");
    __builtin_unreachable();
}

/* Runs on acpi_resume_stack, in protected mode with paging on, after the
 * firmware S3 resume.  The CPU is otherwise freshly reset. */
void acpi_resume_entry(void)
{
    /* Descriptor tables first: printk and every call below need them.
     * init_gdt() rebuilds the table, which also clears the TSS descriptor's
     * busy bit (the boot-time LTR left it set, and a fresh CPU refuses to LTR
     * a busy TSS). */
    init_gdt();
    idt_reload();

    /* CPU state the S3 reset clears: the x87/SSE enables (CR0/CR4) and the
     * SYSCALL/SYSENTER MSRs, without which the syscall return #GPs. */
    (void)fpu_global_init();
    (void)cpu_syscall_commit();

    /* ...and the memory-type state.  The reset drops the MTRRs back to their
     * architectural default, which makes every PCI-hole address (the console
     * framebuffer first) uncacheable, and restores the architectural PAT
     * entry the framebuffer PTEs select.  Both must be back before the first
     * printk, or every console flush costs seconds. */
    mtrr_restore();
    pat_init();
    (void)pat_enable_wc_for_framebuffer((uint32_t)(uintptr_t)fb_get_buffer(),
                                        fb_get_pitch(), fb_get_height());

    pr_notice("  %-11s : S%u resume — reinitialising\n",
              "power", (unsigned)g_suspend_state);

    /* Finish the ACPI wake protocol (_WAK, GPEs, buttons). */
    (void)AcpiLeaveSleepStatePrep((UINT8)g_suspend_state);
    (void)AcpiLeaveSleepState((UINT8)g_suspend_state);

    /* The sleep stops the ACPI PM timer, so re-establish the wall clock before
     * anything waits on it; tsc_hz survives in RAM. */
    ktime_resume();

    /* The interrupt controller was reset with the machine.  Device
     * re-initialisation (PCI/USB/storage) still happens later, in task
     * context — this only has to restore interrupt delivery and the tick. */
    (void)apic_init();

    /* Hand control back to the suspended task. */
    acpi_ctx_restore();
}

/* Stage the trampoline and point the FACS waking vector at it.  MUST run in
 * boot context: ACPI table memory is not reachable from a user page directory,
 * so doing this from the suspend ioctl would silently write nowhere. */
static void stage_wake_trampoline(void)
{
    uint32_t facs_phys = (uint32_t)AcpiGbl_FADT.XFacs;
    const uint8_t *blob = _binary_cact_wake_bin_start;
    uint32_t len = (uint32_t)(_binary_cact_wake_bin_end - _binary_cact_wake_bin_start);
    uint8_t *dst;
    uint32_t *f;

    if (!facs_phys)
        facs_phys = AcpiGbl_FADT.Facs;
    if (!facs_phys || len == 0 || len > 0x1000) {
        pr_err("  %-11s : no FACS (0x%x) or bad trampoline (%u B)\n",
               "power", (unsigned)facs_phys, (unsigned)len);
        return;
    }

    dst = (uint8_t *)acpi_temp_map(WAKE_TRAMP_ADDR, 4096);
    if (!dst) {
        pr_err("  %-11s : trampoline map failed\n", "power");
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        dst[i] = blob[i];
    acpi_temp_unmap(dst, 4096);

    /* Patch the info block (conventional memory is identity mapped). */
    *(volatile uint32_t *)WAKE_INFO_CR3   = read_cr3();
    *(volatile uint32_t *)WAKE_INFO_STACK =
        (uint32_t)(uintptr_t)(acpi_resume_stack + sizeof(acpi_resume_stack));
    *(volatile uint32_t *)WAKE_INFO_ENTRY = (uint32_t)(uintptr_t)acpi_resume_entry;

    f = (uint32_t *)acpi_temp_map(facs_phys, 64);
    if (!f) {
        pr_err("  %-11s : FACS map failed (0x%x)\n", "power", (unsigned)facs_phys);
        return;
    }
    f[3] = WAKE_TRAMP_ADDR;   /* FirmwareWakingVector */
    acpi_temp_unmap(f, 64);

    (void)AcpiSetFirmwareWakingVector((ACPI_PHYSICAL_ADDRESS)WAKE_TRAMP_ADDR, 0);
    pr_info("  %-11s : S3 trampoline @0x%x entry=0x%x cr3=0x%x stack=0x%x\n",
            "power", (unsigned)WAKE_TRAMP_ADDR,
            (unsigned)*(volatile uint32_t *)WAKE_INFO_ENTRY,
            (unsigned)*(volatile uint32_t *)WAKE_INFO_CR3,
            (unsigned)*(volatile uint32_t *)WAKE_INFO_STACK);
}

/* ---------------------------------------------------------------------------
 * CPU-level fallbacks
 * ------------------------------------------------------------------------- */

static void cpu_stop(void) __attribute__((noreturn));
static void cpu_stop(void)
{
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* Load a null IDT and raise an exception: the double->triple fault resets the
 * CPU when no firmware reset path answered. */
static void triple_fault(void) __attribute__((noreturn));
static void triple_fault(void)
{
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } idtr = { 0, 0 };
    __asm__ volatile ("lidt %0" :: "m"(idtr));
    __asm__ volatile ("int $0x03");
    cpu_stop();
}

static void kbc_reset(void)
{
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBC_STATUS_PORT) & KBC_IBF) == 0)
            break;
    }
    outb(KBC_STATUS_PORT, KBC_RESET_CMD);
}

static void spin_ms(unsigned ms)
{
    /* ktime is TSC/PM-timer backed and available before ring 3. */
    ktime_busy_wait_us((uint64_t)ms * 1000u);
}

/* Program PM1a/PM1b directly with the \_Sx SLP_TYP values.  Returns 1 when at
 * least the PM1a control block was written. */
static int pm1_write_direct(uint8_t state)
{
    UINT8 ta = 0, tb = 0;
    uint32_t a, b;

    if (!pm_ready)
        return 0;
    if (ACPI_FAILURE(AcpiGetSleepTypeData(state, &ta, &tb)))
        return 0;

    a = AcpiGbl_FADT.Pm1aControlBlock;
    b = AcpiGbl_FADT.Pm1bControlBlock;
    if (!a)
        return 0;

    outw((uint16_t)a, (uint16_t)(((uint32_t)ta << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
    if (b)
        outw((uint16_t)b, (uint16_t)(((uint32_t)tb << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
    return 1;
}

int acpi_sleep_states(uint32_t *mask)
{
    uint32_t m = 0;

    if (pm_ready) {
        for (uint8_t s = ACPI_STATE_S0; s <= ACPI_STATE_S5; s++) {
            UINT8 a, b;
            if (ACPI_SUCCESS(AcpiGetSleepTypeData(s, &a, &b)))
                m |= (1u << s);
        }
    }
    if (mask)
        *mask = m;
    return pm_ready ? 0 : -1;
}

int acpi_pm_init(void)
{
    if (!acpi_available()) {
        pr_warn("  %-11s : no ACPI — power paths fall back to port I/O\n", "power");
        return -1;
    }

    pm_ready = 1;
    acpi_sleep_states(&s_state_mask);

    /* Boot context (kernel PD): the ACPI table window is readable here, unlike
     * in a later user-process syscall, so stage the resume path now. */
    stage_wake_trampoline();

    pr_info("  %-11s : PM1a_CNT=0x%x reset-reg=%s sleep=[%s%s%s%s%s]\n",
            "power",
            (unsigned)AcpiGbl_FADT.Pm1aControlBlock,
            (AcpiGbl_FADT.Flags & ACPI_FADT_RESET_REGISTER) ? "yes" : "no",
            (s_state_mask & (1u << 1)) ? "S1 " : "",
            (s_state_mask & (1u << 2)) ? "S2 " : "",
            (s_state_mask & (1u << 3)) ? "S3 " : "",
            (s_state_mask & (1u << 4)) ? "S4 " : "",
            (s_state_mask & (1u << 5)) ? "S5" : "");
    return 0;
}

int acpi_suspend(uint32_t state)
{
    ACPI_STATUS st;

    if (state < ACPI_STATE_S1 || state > ACPI_STATE_S4)
        return -1;
    /* S4 needs a hibernation image on disk, which the kernel does not write
     * yet; S2 is only kept for spec completeness. */
    if (state == ACPI_STATE_S2 || state == ACPI_STATE_S4)
        return -1;
    if (!pm_ready || !(s_state_mask & (1u << state))) {
        pr_warn("  %-11s : S%u not advertised by firmware\n", "power", (unsigned)state);
        return -1;
    }

    g_suspend_state = state;

    pr_notice("  %-11s : entering S%u — wake with the power button\n",
              "power", (unsigned)state);

    /* Arm the power button as a wake source; QEMU additionally wakes on the
     * monitor's "system_wakeup". */
    (void)AcpiWriteBitRegister(ACPI_BITREG_POWER_BUTTON_ENABLE, ACPI_ENABLE_EVENT);

    /* Prep must run with interrupts enabled; EnterSleepState with them off. */
    st = AcpiEnterSleepStatePrep((UINT8)state);
    if (ACPI_FAILURE(st)) {
        pr_err("  %-11s : S%u prepare failed (0x%x)\n",
               "power", (unsigned)state, (unsigned)st);
        return -1;
    }

    /* Save the resume context and enter the sleep state.  acpi_do_suspend
     * returns 0 after an S3 wake and -1 when the platform did not sleep; on
     * wake acpi_resume_entry has already reinitialised the machine. */
    if (acpi_do_suspend(state) != 0) {
        pr_err("  %-11s : S%u did not suspend\n", "power", (unsigned)state);
        return -1;
    }

    pr_notice("  %-11s : S%u suspend complete\n", "power", (unsigned)g_suspend_state);
    return 0;
}

void acpi_power_off(void)
{
    int prep_ok = 0;

    pr_notice("  %-11s : poweroff (S5)\n", "power");

    if (pm_ready)
        prep_ok = ACPI_SUCCESS(AcpiEnterSleepStatePrep(ACPI_STATE_S5));

    __asm__ volatile ("cli");   /* no scheduler, no IRQs past this point */

    if (prep_ok) {
        (void)AcpiEnterSleepState(ACPI_STATE_S5);
        pr_warn("  %-11s : S5 returned without powering off\n", "power");
    }

    if (pm1_write_direct(ACPI_STATE_S5)) {
        spin_ms(2000);
        pr_warn("  %-11s : direct PM1 write did not power off\n", "power");
    }

    /* QEMU/Bochs convention: SLP_EN|SLP_TYP=0 on the well-known PM1a ports. */
    outw(QEMU_PM1A_CNT,  (uint16_t)PM1_SLP_EN);
    outw(BOCHS_PM1A_CNT, (uint16_t)PM1_SLP_EN);

    spin_ms(2000);
    pr_err("  %-11s : poweroff unavailable — system halted\n", "power");
    cpu_stop();
}

void acpi_reboot(void)
{
    pr_notice("  %-11s : reboot (reset-reg=0x%x val=0x%x)\n",
              "power",
              (unsigned)AcpiGbl_FADT.ResetRegister.Address,
              (unsigned)AcpiGbl_FADT.ResetValue);
    __asm__ volatile ("cli");

    if (pm_ready && ACPI_SUCCESS(AcpiReset())) {
        pr_notice("  %-11s : reset via the ACPI reset register\n", "power");
        spin_ms(2000);
    }

    /* Reset-control register (PIIX4/ICH9): bit1 pulse plus bit2 system reset. */
    outb(0xCF9, 0x06);
    spin_ms(500);
    outb(0xCF9, 0x0E);
    spin_ms(500);

    kbc_reset();
    spin_ms(1000);

    pr_warn("  %-11s : reset fallbacks exhausted — triple fault\n", "power");
    triple_fault();
}

void acpi_halt(void)
{
    pr_notice("  %-11s : halt\n", "power");
    cpu_stop();
}
