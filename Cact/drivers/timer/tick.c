#include <stdint.h>
#include "tick.h"
#include "smp.h"

static volatile uint32_t timer_ticks = 0;

void timer_tick(void)
{
    /* Only the master core advances the global tick: worker cores share the
     * LAPIC timer vector, but counting every core's tick would make the system
     * clock run N times too fast.  A worker's tick still drives its own
     * preemption (that is handled in the scheduler, not here). */
    if (smp_self_cpu() != 0)
        return;
    timer_ticks++;
}

uint32_t timer_ticks_get(void)
{
    return timer_ticks;
}
