#ifndef CACT_KTIME_H
#define CACT_KTIME_H

#include <stdint.h>
#include <stdbool.h>

#define ACPI_PM_TIMER_FREQ     3579545

/* Monotonic wall-clock microseconds for kernel timekeeping.  Uses the TSC
 * when it is present, invariant and could be calibrated, otherwise the ACPI
 * PM timer. */
int      ktime_init(void);
bool     ktime_using_tsc(void);
uint64_t ktime_get_usec(void);

/* Busy-wait for at least `us` microseconds with interrupts enabled or
 * disabled (used by the ACPI OSL stall).  Prefers the calibrated TSC, then the
 * ACPI PM timer, and only when neither is usable a raw `pause` loop. */
void     ktime_busy_wait_us(uint64_t us);

/* ACPI PM timer (3.579545 MHz) — raw counter and microsecond fallback. */
int      acpi_pm_timer_init(void);
bool     acpi_pm_timer_is_available(void);
uint32_t acpi_pm_timer_read(void);
uint32_t acpi_pm_timer_delta(uint32_t last, uint32_t now);
uint64_t acpi_pm_timer_get_usec(void);

#endif
