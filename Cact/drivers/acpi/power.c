/*
 * power.c — ACPI system power transitions.
 *
 * Owns the kernel's powerdown paths:
 *   - S5 soft-off    (poweroff)   -> AcpiEnterSleepState(S5)
 *   - reset register (reboot)     -> AcpiReset()
 *   - halt                        -> stop the CPU with interrupts off
 *
 * Every destructive path is layered: the ACPI/ACPICA route first, then a
 * direct PM1a/PM1b register write from \_Sx, then the QEMU/Bochs PM1a port
 * convention, then a CPU-level fallback.  A machine with a broken or absent
 * FADT therefore still powers off / resets instead of hanging forever.
 *
 * Suspend-to-RAM (S3) is not implemented in 2.0.0.
 */

#include "kernel.h"
#include "klib.h"
#include "acpi.h"
#include "cact_acpi.h"
#include "ktime.h"

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

static int pm_ready;         /* ACPICA namespace + FADT usable            */
static uint32_t s_state_mask;/* bit Sx set when the \_Sx object is present */

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
