/* LAPIC timer — the system scheduler tick.
 *
 * Calibrates the LAPIC timer against the ACPI PM timer (3.579545 MHz) and arms
 * it in periodic mode on LAPIC_TIMER_VECTOR.  The HPET is not used.
 */

#include "kernel.h"
#include "klib.h"
#include "apic.h"
#include "ktime.h"
#include "lapic_timer.h"

#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INITCNT 0x380
#define LAPIC_TIMER_CURCNT  0x390
#define LAPIC_TIMER_DIV     0x3E0

#define LAPIC_LVT_MASK      (1u << 16)
#define LAPIC_LVT_PERIODIC  (1u << 17)

static int lapic_timer_armed = 0;

/* Start a one-shot countdown from the maximum count, divide by 1.  The LVT is
 * left as-is: masking only gates interrupt delivery, the counter still runs,
 * which is all the calibration needs. */
static void lapic_timer_start_oneshot(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    lapic[LAPIC_TIMER_DIV / 4] = 0x0B;
    lapic[LAPIC_TIMER_INITCNT / 4] = 0xFFFFFFFFu;
}

static void lapic_timer_stop(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    lapic[LAPIC_TIMER_INITCNT / 4] = 0;
}

/* Calibrate against the ACPI PM timer: measure the LAPIC countdown across a
 * ~50 ms window of the stable 3.579545 MHz reference.  Returns ticks per
 * millisecond, or 0 on failure. */
static uint32_t lapic_calibrate_pm(void)
{
    volatile uint32_t *lapic = apic_lapic_regs();
    if (!acpi_pm_timer_is_available())
        return 0;

    lapic_timer_start_oneshot();

    uint32_t first = lapic[LAPIC_TIMER_CURCNT / 4];

    /* Poll the raw counter, not acpi_pm_timer_get_usec(): the latter takes the
     * shared timekeeping spinlock, and this loop samples thousands of times,
     * which would contend with the wall clock for no benefit. */
    uint32_t target = ACPI_PM_TIMER_FREQ / 20;   /* ~50 ms worth of counts */
    uint32_t prev   = acpi_pm_timer_read();
    uint64_t counts = 0;
    uint32_t guard  = 200000000u;
    while (counts < target && guard != 0) {
        uint32_t now = acpi_pm_timer_read();
        counts += acpi_pm_timer_delta(prev, now);
        prev = now;
        guard--;
    }

    uint32_t last = lapic[LAPIC_TIMER_CURCNT / 4];
    lapic_timer_stop();

    if (guard == 0 || counts == 0)
        return 0;

    uint64_t usec = counts * 1000000ull / ACPI_PM_TIMER_FREQ;
    uint32_t elapsed = first - last;    /* 32-bit modulo handles one wrap */
    if (usec == 0 || elapsed == 0)
        return 0;

    return (uint32_t)((uint64_t)elapsed * 1000ull / usec);
}

uint32_t lapic_timer_calibrate(void)
{
    if (!apic_lapic_regs())
        return 0;

    uint32_t per_ms = lapic_calibrate_pm();
    if (per_ms == 0) {
        pr_warn("  %-11s : calibration failed (ACPI PM timer unavailable)\n",
                "timer");
        return 0;
    }

    {
        char buf[80]; char num[32];
        strcpy(buf, "LAPIC timer: calibrated at ");
        snprintf(num, sizeof(num), "%llu", (unsigned long long)per_ms * 1000ull);
        strcat(buf, num);
        strcat(buf, " Hz (ACPI PM timer)");
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
