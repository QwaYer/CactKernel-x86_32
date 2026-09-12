#include "kernel.h"
#include "klib.h"
#include "acpi.h"
#include "ktime.h"
#include "cact_acpi.h"
#include "sync.h"

/* ---------------------------------------------------------------------------
 * ACPI PM timer (3.579545 MHz) — the fallback wall clock.
 * ------------------------------------------------------------------------ */

static uint16_t pm_timer_port = 0;
static int      pm_timer_32bit = 0;
static int      pm_timer_available = 0;

static uint32_t  last_pm_count = 0;
static volatile uint32_t  pm_overflow_count = 0;
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

    last_pm_count = val;
    pm_overflow_count = 0;
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

uint64_t acpi_pm_timer_get_usec(void)
{
    if (!pm_timer_available) return 0;

    irq_spinlock_acquire(&pm_timer_lock);

    uint32_t val = acpi_pm_timer_read();
    uint32_t max_val = pm_timer_get_max();

    if (val < last_pm_count && (last_pm_count - val) > (max_val / 2)) {
        pm_overflow_count++;
    }
    last_pm_count = val;

    uint64_t total_counts = (uint64_t)pm_overflow_count * (uint64_t)(max_val + 1) + val;

    irq_spinlock_release(&pm_timer_lock);

    return (total_counts * 1000000ull) / ACPI_PM_TIMER_FREQ;
}

/* ---------------------------------------------------------------------------
 * TSC timekeeping — the primary wall clock.
 *
 * The TSC is calibrated at boot against the ACPI PM timer when it exists,
 * otherwise against PIT channel 2 (one-shot, no IRQ).  If the CPU has no
 * TSC (or calibration fails) every ktime_* call falls back to the PM timer.
 * ------------------------------------------------------------------------ */

/* PIT (8254) channel 2, used for calibration when the PM timer is missing. */
#define PIT_CH2_DATA        0x42
#define PIT_CMD             0x43
#define PIT_CH2_CTRL        0x61
#define PIT_GATE2           (1u << 0)
#define PIT_OUT2            (1u << 5)
#define PIT_BASE_FREQ       1193182u

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
        elapsed = (now - start) & max;   /* delta modulo (max + 1) */
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

/* Calibrate the TSC against one PIT channel 2 one-shot (~54.9 ms). */
static int calibrate_tsc_pit(void)
{
    uint8_t ctrl = inb(PIT_CH2_CTRL);
    outb(PIT_CH2_CTRL, ctrl | PIT_GATE2);
    outb(PIT_CMD, 0xB0);                 /* ch2, lobyte+hibyte, mode 0, binary */
    outb(PIT_CH2_DATA, 0xFF);
    outb(PIT_CH2_DATA, 0xFF);

    uint64_t t0 = read_tsc();
    uint32_t guard = 10000000u;
    while (!(inb(PIT_CH2_CTRL) & PIT_OUT2) && guard--)
        __asm__ __volatile__("pause");
    uint64_t t1 = read_tsc();

    if (guard == 0)
        return -1;

    uint64_t period_us = (65535ull * 1000000ull) / PIT_BASE_FREQ;   /* ~54931 */
    tsc_hz = (t1 - t0) * 1000000ull / period_us;
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

    int calib = pm_ok ? calibrate_tsc_pm() : calibrate_tsc_pit();
    if (calib != 0) {
        pr_warn("  %-11s : calibration failed — using ACPI PM timer\n", "tsc");
        tsc_available = 0;
        return pm_ok ? 0 : -1;
    }

    tsc_available = 1;
    pr_info("  %-11s : %u MHz, calibrated against %s\n", "tsc",
            (unsigned)(tsc_hz / 1000000ull), pm_ok ? "PM timer" : "PIT");
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
