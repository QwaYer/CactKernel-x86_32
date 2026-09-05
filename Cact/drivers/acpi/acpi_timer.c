#include "kernel.h"
#include "klib.h"
#include "acpi.h"
#include "acpi_timer.h"
#include "acpi_hpet.h"
#include "cact_acpi.h"
#include "sync.h"

static uint16_t pm_timer_port = 0;
static int      pm_timer_32bit = 0;
static int      pm_timer_available = 0;

static uint32_t  last_pm_count = 0;
static volatile uint32_t  pm_overflow_count = 0;
static volatile uint32_t  timer_ticks = 0;
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
    timer_ticks = 0;
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

void acpi_pm_timer_tick(void)
{
    timer_ticks++;
}

uint32_t timer_ticks_get(void)
{
    return timer_ticks;
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
