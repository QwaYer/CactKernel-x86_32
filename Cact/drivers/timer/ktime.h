#ifndef CACT_ACPI_TIMER_H
#define CACT_ACPI_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#define ACPI_PM_TIMER_FREQ     3579545

int  acpi_pm_timer_init(void);
bool acpi_pm_timer_is_available(void);
void acpi_pm_timer_tick(void);
uint32_t acpi_pm_timer_read(void);
uint64_t acpi_pm_timer_get_usec(void);
uint32_t timer_ticks_get(void);

#endif
