#include <stdint.h>
#include "tick.h"

static volatile uint32_t timer_ticks = 0;

void timer_tick(void)
{
    timer_ticks++;
}

uint32_t timer_ticks_get(void)
{
    return timer_ticks;
}
