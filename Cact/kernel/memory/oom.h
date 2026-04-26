#ifndef OOM_H
#define OOM_H

#include <stdint.h>

typedef struct {
    uint32_t oom_kills;
    uint32_t pages_reclaimed;
    uint32_t last_killed_pid;
} oom_stats_t;

int         oom_kill(void);
oom_stats_t oom_get_stats(void);
void        oom_print_stats(void);

#endif
