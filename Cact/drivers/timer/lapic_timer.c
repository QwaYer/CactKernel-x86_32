/* LAPIC timer fallback (manufacturer quirk).
 *
 * The Intel Coffee Lake / B360 chipset HPET is broken — Linux forces it off
 * and uses the TSC/LAPIC timer instead.  This module calibrates the LAPIC
 * timer against the PIT (8254) channel 2 and arms it in periodic mode on the
 * standard timer vector (0x20), so the system keeps ticking even when the
 * HPET cannot be trusted.
 */

#include "kernel.h"
#include "klib.h"
#include "pci.h"
#include "apic.h"
#include "acpi_hpet.h"
#include "lapic_timer.h"

#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INITCNT 0x380
#define LAPIC_TIMER_CURCNT  0x390
#define LAPIC_TIMER_DIV     0x3E0

#define LAPIC_LVT_MASK      (1u << 16)
#define LAPIC_LVT_PERIODIC  (1u << 17)

#define PIT_CH2_DATA        0x42
#define PIT_CMD             0x43
#define PIT_CH2_CTRL        0x61
#define PIT_GATE2           (1u << 0)
#define PIT_OUT2            (1u << 5)
#define PIT_BASE_FREQ       1193182u

static int lapic_timer_armed = 0;

uint32_t lapic_timer_calibrate(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    if (!lapic)
        return 0;

    /* PIT channel 2: one-shot, count 0xFFFF (~54.9 ms). No IRQ involved. */
    uint8_t ctrl = inb(PIT_CH2_CTRL);
    outb(PIT_CH2_CTRL, ctrl | PIT_GATE2);
    outb(PIT_CMD, 0xB0);                 /* ch2, lobyte+hibyte, mode 0, binary */
    outb(PIT_CH2_DATA, 0xFF);
    outb(PIT_CH2_DATA, 0xFF);

    /* LAPIC timer: divide by 1, one-shot with the maximum count. */
    lapic[LAPIC_TIMER_DIV / 4] = 0x0B;
    lapic[LAPIC_TIMER_INITCNT / 4] = 0xFFFFFFFFu;

    /* Wait for PIT to reach zero (bit 5 = OUT2 high), bounded. */
    uint32_t guard = 10000000u;
    while (!(inb(PIT_CH2_CTRL) & PIT_OUT2) && guard--)
        __asm__ __volatile__("pause");

    uint32_t remaining = lapic[LAPIC_TIMER_CURCNT / 4];
    lapic[LAPIC_TIMER_INITCNT / 4] = 0;             /* stop */

    if (guard == 0) {
        pr_warn("  %-11s : PIT calibration timed out\n", "timer");
        return 0;
    }

    uint64_t elapsed   = 0xFFFFFFFFull - remaining;
    uint64_t period_us = (65535ull * 1000000ull) / PIT_BASE_FREQ;   /* ~54931 */
    uint32_t per_ms    = (uint32_t)((elapsed * 1000ull) / period_us);

    if (per_ms == 0) {
        pr_warn("  %-11s : calibration failed\n", "timer");
        return 0;
    }

    {
        char buf[64]; char num[32];
        strcpy(buf, "LAPIC timer: calibrated at ");
        snprintf(num, sizeof(num), "%d", (int)(per_ms * 1000)); strcat(buf, num);
        strcat(buf, " Hz");
        pr_info("%s", buf);
    }
    return per_ms;
}

void lapic_timer_start_periodic(uint32_t ticks_per_ms)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    if (!lapic || ticks_per_ms == 0)
        return;

    /* 100 Hz tick (10 ms) to match the HPET-driven tick rate. */
    uint32_t count = ticks_per_ms * 10u;

    /* Program masked first, then unmask to avoid a spurious edge. */
    lapic[LAPIC_LVT_TIMER / 4] = 0x20 | LAPIC_LVT_PERIODIC | LAPIC_LVT_MASK;
    lapic[LAPIC_TIMER_DIV / 4] = 0x0B;
    lapic[LAPIC_TIMER_INITCNT / 4] = count;
    lapic[LAPIC_LVT_TIMER / 4] = 0x20 | LAPIC_LVT_PERIODIC;

    lapic_timer_armed = 1;
    pr_info("LAPIC timer: periodic 100 Hz armed");
}

bool lapic_timer_active(void)
{
    return lapic_timer_armed != 0;
}

bool lapic_timer_force_hpet_off(void)
{
    /* Manufacturer quirk: the Intel 300-series PCH HPET is dysfunctional —
     * Linux forces it off.  Detect the PCH LPC/eSPI device (fixed BDF
     * 00:1F.0) directly.  The 300-series desktops span two ID families:
     *   Z370/H370/H310/B360/B365/Q370  → 0xA2C8..0xA2CF (LPC 0xA2CC on B360)
     *   Z390 and late steppings         → 0xA300..0xA30F
     * On any other chipset the HPET stays primary and this returns false. */
    uint32_t id      = pci_read_config_dword(0, 0x1F, 0, 0x00);
    uint16_t vendor  = (uint16_t)(id & 0xFFFF);
    uint16_t device  = (uint16_t)(id >> 16);

    if (vendor == 0x8086 &&
        (((device & 0xFFF0) == 0xA300) ||   /* late 300-series (Z390 etc.) */
         ((device & 0xFFF8) == 0xA2C8)))    /* Z370/B360/H370/H310/B365 */
        return 1;
    return 0;
}

int lapic_timer_select_source(void)
{
    if (lapic_timer_force_hpet_off()) {
        pr_warn("  %-11s : HPET disabled by board quirk — LAPIC timer used\n", "timer");
        return 0;
    }

    if (hpet_init() != 0) {
        pr_warn("  %-11s : HPET init failed — LAPIC timer fallback used\n", "timer");
        return 0;
    }

    pr_info("  %-11s : HPET selected as system clock\n", "timer");
    return 0;
}
