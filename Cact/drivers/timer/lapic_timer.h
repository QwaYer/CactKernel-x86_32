#ifndef CACT_LAPIC_TIMER_H
#define CACT_LAPIC_TIMER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * LAPIC timer — the system scheduler tick.
 *
 * The LAPIC timer is the only periodic interrupt source: it is calibrated
 * against the PIT (8254) channel 2 and armed in periodic mode on the
 * standard timer vector (0x20), which device_isrs.asm dispatches to the
 * scheduler.  The HPET is not used.
 */

/* Calibrate the LAPIC timer against PIT channel 2.
 * Returns ticks per millisecond, or 0 on failure. */
uint32_t lapic_timer_calibrate(void);

/* Arm the LAPIC timer in periodic mode at 100 Hz (10 ms tick) on vector 0x20. */
void lapic_timer_start_periodic(uint32_t ticks_per_ms);

bool lapic_timer_active(void);

#endif
