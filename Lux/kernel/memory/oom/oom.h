#ifndef OOM_H
#define OOM_H

#include <stdint.h>

typedef struct {
    uint32_t oom_kills;
    uint32_t pages_reclaimed;
    uint32_t last_killed_pid;
} oom_stats_t;

/* Try to free memory by killing the largest user process.
 * Returns 0 on success (a process was killed and pages reclaimed),
 * -1 if no suitable victim was found. */
int oom_kill(void);

oom_stats_t oom_get_stats(void);
void        oom_print_stats(void);

#endif
