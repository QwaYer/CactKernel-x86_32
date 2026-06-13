#include "kernel.h"
#include "acpi.h"
#include "acpi_timer.h"
#include "acpi_hpet.h"
#include "cact_acpi.h"
#include <string.h>

static uint16_t pm_timer_port = 0;
static int      pm_timer_32bit = 0;
static int      pm_timer_available = 0;

static uint32_t  last_pm_count = 0;
static volatile uint32_t  pm_overflow_count = 0;
static volatile uint32_t  timer_ticks = 0;

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
        klog(LOG_WARN, "ACPI PM timer: no PM timer block in FADT");
        return -1;
    }

    if (fadt->XPmTimerBlock.Address &&
        fadt->XPmTimerBlock.SpaceId == ACPI_ADR_SPACE_SYSTEM_IO &&
        fadt->XPmTimerBlock.BitWidth >= 32) {
        pm_timer_port = (uint16_t)fadt->XPmTimerBlock.Address;
    } else if (fadt->PmTimerBlock != 0) {
        pm_timer_port = (uint16_t)fadt->PmTimerBlock;
    } else {
        klog(LOG_WARN, "ACPI PM timer: unsupported address space");
        return -1;
    }

    if (pm_timer_port == 0) {
        klog(LOG_WARN, "ACPI PM timer: invalid port 0");
        return -1;
    }

    pm_timer_32bit = (fadt->Flags & ACPI_FADT_32BIT_TIMER) ? 1 : 0;

    uint32_t val = pm_timer_read_port();
    val &= pm_timer_get_max();

    last_pm_count = val;
    pm_overflow_count = 0;
    timer_ticks = 0;
    pm_timer_available = 1;

    char buf[64];
    char hex[16];
    strcpy(buf, "ACPI PM timer: port 0x");
    hex_to_ascii(pm_timer_port, hex);
    strcat(buf, hex);
    strcat(buf, pm_timer_32bit ? " (32-bit)" : " (24-bit)");
    klog(LOG_OK, buf);

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

    uint32_t val = acpi_pm_timer_read();
    uint32_t max_val = pm_timer_get_max();
    uint64_t total_counts;
    uint32_t current_overflow;

    if (val < last_pm_count && (last_pm_count - val) > (max_val / 2)) {
        current_overflow = pm_overflow_count + 1;
    } else {
        current_overflow = pm_overflow_count;
    }

    total_counts = (uint64_t)current_overflow * (uint64_t)(max_val + 1) + val;
    return (total_counts * 1000000ull) / ACPI_PM_TIMER_FREQ;
}
