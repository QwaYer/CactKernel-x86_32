#ifndef CACT_LAPIC_TIMER_H
#define CACT_LAPIC_TIMER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * LAPIC timer — the system scheduler tick.
 *
 * The LAPIC timer is the only periodic interrupt source: it is calibrated
 * against the ACPI PM timer (3.579545 MHz) and armed in periodic mode on
 * LAPIC_TIMER_VECTOR (0xFE, see kernel.h), which device_isrs.asm dispatches to
 * the scheduler.  The HPET is not used.
 */

/* Calibrate the LAPIC timer against the ACPI PM timer.
 * Returns ticks per millisecond, or 0 on failure. */
uint32_t lapic_timer_calibrate(void);

/* Arm the LAPIC timer in periodic mode at 100 Hz (10 ms tick) on
 * LAPIC_TIMER_VECTOR. */
void lapic_timer_start_periodic(uint32_t ticks_per_ms);

/* Stop delivering LAPIC timer interrupts (LVT masked, counter left running). */
void lapic_timer_mask(void);

/* Sanity-check the armed timer: LVT vector/mode/mask and a moving current
 * count.  Returns 0 when healthy, -1 when something looks wrong (the failure
 * is logged). */
int lapic_timer_selftest(void);

bool lapic_timer_active(void);

#endif
