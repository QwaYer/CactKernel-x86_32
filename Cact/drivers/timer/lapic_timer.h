#ifndef CACT_QUIRKS_TIMER_LAPIC_H
#define CACT_QUIRKS_TIMER_LAPIC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * LAPIC timer fallback — manufacturer quirk: the Intel Coffee Lake / B360
 * chipset HPET is broken (Linux disables it).  When the HPET cannot drive
 * the system tick, calibrate the LAPIC timer against the PIT (8254) and arm
 * it in periodic mode on the standard timer vector (0x20).
 */

/* Calibrate the LAPIC timer against PIT channel 2.
 * Returns ticks per millisecond, or 0 on failure. */
uint32_t lapic_timer_calibrate(void);

/* Arm the LAPIC timer in periodic mode at 100 Hz (10 ms tick) on vector 0x20 —
 * the same gate the rest of the kernel expects for the timer IRQ. */
void lapic_timer_start_periodic(uint32_t ticks_per_ms);

bool lapic_timer_active(void);

/* Manufacturer quirk: true when the chipset HPET must not be used at all
 * (Intel Coffee Lake / B360), forcing the LAPIC timer as the system ticker. */
bool lapic_timer_force_hpet_off(void);

/* Decide the system timer source (HPET vs LAPIC fallback) and log it.
 * The actual ticker is armed in apic_init() once the APIC is up.
 * Returns 0 on success, -1 if no timer source is available. */
int lapic_timer_select_source(void);

#endif
