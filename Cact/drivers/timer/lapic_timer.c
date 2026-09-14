/* LAPIC timer — the system scheduler tick.
 *
 * Calibrates the LAPIC timer against the PIT (8254) channel 2 and arms it in
 * periodic mode on the standard timer vector (0x20).  The HPET is not used.
 */

#include "kernel.h"
#include "klib.h"
#include "apic.h"
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
#define PIT_SPKR            (1u << 1)
#define PIT_OUT2            (1u << 5)
#define PIT_BASE_FREQ       1193182u

static int lapic_timer_armed = 0;

uint32_t lapic_timer_calibrate(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    if (!lapic)
        return 0;

    /* PIT channel 2: one-shot, count 0xFFFF (~54.9 ms). No IRQ involved.
     * Lower the gate first so the counter cannot start on a partially loaded
     * value on real silicon, load mode + count, then raise the gate. */
    uint8_t ctrl = inb(PIT_CH2_CTRL);
    outb(PIT_CH2_CTRL, ctrl & ~PIT_GATE2);
    outb(PIT_CMD, 0xB0);                 /* ch2, lobyte+hibyte, mode 0, binary */
    outb(PIT_CH2_DATA, 0xFF);
    outb(PIT_CH2_DATA, 0xFF);
    outb(PIT_CH2_CTRL, (uint8_t)((ctrl & ~PIT_SPKR) | PIT_GATE2));

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

    /* 100 Hz tick (10 ms) — the scheduler quantum base. */
    uint32_t count = ticks_per_ms * 10u;

    /* Program masked first, then unmask to avoid a spurious edge. */
    lapic[LAPIC_LVT_TIMER / 4] =
        LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC | LAPIC_LVT_MASK;
    lapic[LAPIC_TIMER_DIV / 4] = 0x0B;
    lapic[LAPIC_TIMER_INITCNT / 4] = count;
    lapic[LAPIC_LVT_TIMER / 4] = LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC;

    lapic_timer_armed = 1;
    pr_info("LAPIC timer: periodic 100 Hz armed");
}

void lapic_timer_mask(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    if (!lapic)
        return;
    lapic[LAPIC_LVT_TIMER / 4] =
        LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC | LAPIC_LVT_MASK;
    lapic_timer_armed = 0;
}

int lapic_timer_selftest(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    if (!lapic) {
        pr_warn("  %-11s : selftest: no LAPIC mapping\n", "timer");
        return -1;
    }

    /* Vector + periodic + unmasked. */
    const uint32_t fields = 0xFFu | LAPIC_LVT_MASK | LAPIC_LVT_PERIODIC;
    const uint32_t expect = LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC;
    uint32_t lvt = lapic[LAPIC_LVT_TIMER / 4];
    if ((lvt & fields) != expect) {
        pr_warn("  %-11s : selftest: LVT=0x%x (expected 0x%x) — vector/mask wrong\n",
                "timer", (unsigned)lvt, (unsigned)expect);
        return -1;
    }

    /* The counter must be moving.  Periodic mode reloads, so a stuck value
     * means the timer is not running at all. */
    uint32_t a = lapic[LAPIC_TIMER_CURCNT / 4];
    for (volatile uint32_t i = 0; i < 1000000u; i++)
        __asm__ __volatile__("pause");
    uint32_t b = lapic[LAPIC_TIMER_CURCNT / 4];
    if (a == b) {
        pr_warn("  %-11s : selftest: LAPIC counter stuck at 0x%x — timer not running\n",
                "timer", (unsigned)a);
        return -1;
    }

    pr_info("  %-11s : selftest OK (vector 0x%x, CURCNT 0x%x->0x%x)\n",
            "timer", (unsigned)LAPIC_TIMER_VECTOR, (unsigned)a, (unsigned)b);
    return 0;
}

bool lapic_timer_active(void)
{
    return lapic_timer_armed != 0;
}
