#include "kernel.h"
#include "klib.h"
#include "acpi.h"
#include "ktime.h"
#include "cact_acpi.h"
#include "cpudev.h"
#include "sync.h"

/* ---------------------------------------------------------------------------
 * ACPI PM timer (3.579545 MHz) — the fallback wall clock.
 * ------------------------------------------------------------------------ */

static uint16_t pm_timer_port = 0;
static int      pm_timer_32bit = 0;
static int      pm_timer_available = 0;

/* Delta accumulation instead of "count + overflow flag": subtracting the last
 * sample from the current one modulo the counter width handles a single wrap
 * with no polling-rate assumption at all.  The exact count total is kept in
 * 64 bits so microsecond conversion only truncates once, on read. */
static uint32_t  pm_last_count = 0;
static uint64_t  pm_total_counts = 0;
static irq_spinlock_t pm_timer_lock;

#define PM_TIMER_24BIT_MASK  0x00FFFFFFu
#define PM_TIMER_32BIT_MASK  0xFFFFFFFFu

static inline uint32_t pm_timer_get_max(void)
{
    return pm_timer_32bit ? PM_TIMER_32BIT_MASK : PM_TIMER_24BIT_MASK;
}

static inline uint32_t pm_timer_read_port(void)
{
    return port_dword_in(pm_timer_port);
}

int acpi_pm_timer_init(void)
{
    ACPI_TABLE_FADT *fadt = &AcpiGbl_FADT;

    if (fadt->PmTimerBlock == 0 && fadt->XPmTimerBlock.Address == 0) {
        pr_warn("  %-11s : no PM timer block in FADT\n", "pm-timer");
        return -1;
    }

    if (fadt->XPmTimerBlock.Address &&
        fadt->XPmTimerBlock.SpaceId == ACPI_ADR_SPACE_SYSTEM_IO &&
        fadt->XPmTimerBlock.BitWidth >= 32) {
        pm_timer_port = (uint16_t)fadt->XPmTimerBlock.Address;
    } else if (fadt->PmTimerBlock != 0) {
        pm_timer_port = (uint16_t)fadt->PmTimerBlock;
    } else {
        pr_warn("  %-11s : unsupported address space\n", "pm-timer");
        return -1;
    }

    if (pm_timer_port == 0) {
        pr_warn("  %-11s : invalid port 0\n", "pm-timer");
        return -1;
    }

    pm_timer_32bit = (fadt->Flags & ACPI_FADT_32BIT_TIMER) ? 1 : 0;

    uint32_t val = pm_timer_read_port();
    val &= pm_timer_get_max();

    pm_last_count = val;
    pm_total_counts = 0;
    irq_spinlock_init(&pm_timer_lock);
    pm_timer_available = 1;

    char buf[96];
    snprintf(buf, sizeof(buf), "  %-11s : timekeeping ready (port 0x%x, %s)\n",
             "pm-timer", (unsigned)pm_timer_port,
             pm_timer_32bit ? "32-bit" : "24-bit");
    pr_info("%s", buf);

    return 0;
}

bool acpi_pm_timer_is_available(void)
{
    return pm_timer_available != 0;
}

uint32_t acpi_pm_timer_read(void)
{
    if (!pm_timer_available) return 0;
    return pm_timer_read_port() & pm_timer_get_max();
}

/* Wrap-safe distance from `last` to `now` on the PM counter. */
uint32_t acpi_pm_timer_delta(uint32_t last, uint32_t now)
{
    uint32_t max_val = pm_timer_get_max();

    /* A 32-bit counter wraps at max_val + 1 == 0, so the `+ 1` below would
     * overflow; plain modular subtraction already gives the right distance. */
    if (max_val == PM_TIMER_32BIT_MASK)
        return now - last;

    return (now >= last) ? (now - last) : (max_val - last + now + 1);
}

uint64_t acpi_pm_timer_get_usec(void)
{
    if (!pm_timer_available) return 0;

    irq_spinlock_acquire(&pm_timer_lock);

    uint32_t val = pm_timer_read_port() & pm_timer_get_max();
    pm_total_counts += acpi_pm_timer_delta(pm_last_count, val);
    pm_last_count = val;

    uint64_t total = pm_total_counts;

    irq_spinlock_release(&pm_timer_lock);

    /* Split the conversion so `total * 1000000` cannot overflow even after
     * months of uptime (a single 64-bit multiply wraps after ~58 days). */
    uint64_t secs = total / ACPI_PM_TIMER_FREQ;
    uint64_t rem  = total % ACPI_PM_TIMER_FREQ;
    return secs * 1000000ull + (rem * 1000000ull) / ACPI_PM_TIMER_FREQ;
}

/* ---------------------------------------------------------------------------
 * TSC timekeeping — the primary wall clock.
 *
 * The TSC is calibrated at boot against the ACPI PM timer when it exists.
 * When the FADT exposes no PM timer block, the nominal TSC frequency from
 * CPUID (leaves 0x15/0x16) is used instead: no hardware timer is driven.
 * If the CPU has no TSC (or calibration fails) every ktime_* call falls back
 * to the PM timer.
 * ------------------------------------------------------------------------ */

static uint64_t tsc_hz = 0;
static int      tsc_available = 0;

static inline uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static int tsc_supported(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0));
    return (edx & (1u << 4)) != 0;      /* CPUID.01H:EDX.TSC */
}

/* Calibrate the TSC against the PM timer over a ~50 ms window. */
static int calibrate_tsc_pm(void)
{
    uint32_t max = pm_timer_get_max();
    uint32_t target = ACPI_PM_TIMER_FREQ / 20;   /* ~50 ms worth of counts */
    if (target == 0 || target >= max)
        target = max / 4;

    uint32_t start = pm_timer_read_port() & max;
    uint64_t t0 = read_tsc();

    uint32_t elapsed = 0;
    uint32_t guard = 200000000u;
    do {
        uint32_t now = pm_timer_read_port() & max;
        elapsed = acpi_pm_timer_delta(start, now);
        if (elapsed >= target) break;
        __asm__ __volatile__("pause");
    } while (guard--);

    uint64_t t1 = read_tsc();

    if (guard == 0 || elapsed == 0)
        return -1;

    uint64_t usec = (uint64_t)elapsed * 1000000ull / ACPI_PM_TIMER_FREQ;
    if (usec == 0)
        return -1;

    tsc_hz = (t1 - t0) * 1000000ull / usec;
    return tsc_hz ? 0 : -1;
}

int ktime_init(void)
{
    int pm_ok = (acpi_pm_timer_init() == 0);

    if (!tsc_supported()) {
        pr_warn("  %-11s : no TSC — using ACPI PM timer for timekeeping\n", "tsc");
        tsc_available = 0;
        return pm_ok ? 0 : -1;
    }

    int calib = -1;
    const char *reference = "CPUID";
    if (pm_ok && calibrate_tsc_pm() == 0) {
        calib = 0;
        reference = "PM timer";
    } else {
        /* No PM timer (or its calibration failed): trust the frequency
         * firmware enumerates in CPUID rather than driving the 8254 PIT. */
        tsc_hz = cpu_tsc_hz_from_cpuid();
        if (tsc_hz != 0)
            calib = 0;
    }
    if (calib != 0) {
        pr_warn("  %-11s : calibration failed — using ACPI PM timer\n", "tsc");
        tsc_available = 0;
        return pm_ok ? 0 : -1;
    }

    /* A non-invariant TSC stops in deep C-states and shifts with P-state
     * changes, so a resume can read a stale value and time jumps backwards.
     * Keep it for the calibrated busy-wait, but only make it the wall clock
     * when firmware reports it as constant-rate. */
    int invariant = cpu_has_invariant_tsc();

    if (!invariant && pm_ok) {
        tsc_available = 0;
        pr_warn("  %-11s : %u MHz, no invariant TSC — ACPI PM timer is the wall clock\n",
                "tsc", (unsigned)(tsc_hz / 1000000ull));
        return 0;
    }

    tsc_available = 1;
    if (!invariant)
        pr_warn("  %-11s : %u MHz, no invariant TSC and no PM timer — TSC is the only clock\n",
                "tsc", (unsigned)(tsc_hz / 1000000ull));
    else
        pr_info("  %-11s : %u MHz, invariant, calibrated against %s\n", "tsc",
                (unsigned)(tsc_hz / 1000000ull), reference);
    return 0;
}

bool ktime_using_tsc(void)
{
    return tsc_available != 0;
}

uint64_t ktime_get_usec(void)
{
    if (!tsc_available)
        return acpi_pm_timer_get_usec();

    uint64_t t = read_tsc();
    uint64_t secs = t / tsc_hz;
    uint64_t rem  = t % tsc_hz;
    return secs * 1000000ull + (rem * 1000000ull) / tsc_hz;
}

/* ---------------------------------------------------------------------------
 * Busy-wait — the OSL delay primitive behind AcpiOsStall.
 *
 * The TSC gives an exact cycle count independent of CPU frequency changes, so
 * it is preferred whenever calibrated (a busy loop keeps the core awake, so a
 * non-invariant TSC is still fine here).  Next comes the PM timer.  Only when
 * neither is usable — no TSC and no PM timer — is a raw `pause` loop used,
 * and then there is no reference clock left to calibrate it against.
 * ------------------------------------------------------------------------ */

/* `pause` iterations per microsecond for the degenerate no-clock case.  The
 * count is deliberately high so a stall overshoots rather than undershoots. */
#define PAUSE_PER_US_FALLBACK  2000u

static void pause_delay_us(uint64_t us)
{
    uint64_t total = us * (uint64_t)PAUSE_PER_US_FALLBACK;
    while (total) {
        uint32_t chunk = (total > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)total;
        for (uint32_t i = 0; i < chunk; i++)
            __asm__ __volatile__("pause" ::: "memory");
        total -= chunk;
    }
}

void ktime_busy_wait_us(uint64_t us)
{
    if (us == 0)
        return;

    if (tsc_hz != 0) {
        /* Split the product so `us * tsc_hz` cannot overflow 64 bits. */
        uint64_t ticks = (us / 1000000ull) * tsc_hz
                       + ((us % 1000000ull) * tsc_hz) / 1000000ull;
        if (ticks == 0)
            ticks = 1;
        uint64_t start = read_tsc();
        while (read_tsc() - start < ticks)
            __asm__ __volatile__("pause" ::: "memory");
        return;
    }

    if (pm_timer_available) {
        uint64_t start = acpi_pm_timer_get_usec();
        while (acpi_pm_timer_get_usec() - start < us)
            __asm__ __volatile__("pause" ::: "memory");
        return;
    }

    pause_delay_us(us);
}
