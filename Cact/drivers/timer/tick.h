#ifndef CACT_TIMER_TICK_H
#define CACT_TIMER_TICK_H

#include <stdint.h>

/* Scheduler tick counter ("jiffies"): advanced once per LAPIC timer
 * interrupt (LAPIC_TIMER_VECTOR) and used as the kernel's coarse time base
 * for timeouts and sleep deadlines. */
void     timer_tick(void);
uint32_t timer_ticks_get(void);

#endif
