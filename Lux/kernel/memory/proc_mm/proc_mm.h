#ifndef PROCESS_MM_H
#define PROCESS_MM_H

#include <stdint.h>
#include "memory.h"

#define PROC_MAX_PAGES 256

typedef struct {
    void*    pages[PROC_MAX_PAGES]; /* физические адреса страниц данных/кода */
    uint32_t count;                 /* реально использованных записей         */
    uint32_t* page_dir;             /* пользовательский page directory        */
} proc_page_tracker_t;

static inline void proc_tracker_init(proc_page_tracker_t* t)
{
    for (uint32_t i = 0; i < PROC_MAX_PAGES; i++)
        t->pages[i] = 0;
    t->count    = 0;
    t->page_dir = 0;
}

static inline int proc_tracker_add(proc_page_tracker_t* t, void* phys)
{
    if (t->count >= PROC_MAX_PAGES)
        return -1;
    t->pages[t->count++] = phys;
    return 0;
}

void proc_free_pages(proc_page_tracker_t* t);

#endif /* PROCESS_MM_H */